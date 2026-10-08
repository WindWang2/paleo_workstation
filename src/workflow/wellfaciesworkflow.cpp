// 层：功能
#include "wellfaciesworkflow.h"
#include "ai/wellfacieskeystore.h"
#include "../catalog/datacatalog.h"
#include "derivedassets.h"
#include "../qgis/wellattributestore.h"
#include "../qgis/qgislayerservice.h"
#include <qgsvectorlayer.h>
#include <QTimer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

WellFaciesWorkflow::WellFaciesWorkflow(QObject *parent) : QObject(parent) {
  m_config = WellFaciesConfig::load();
  m_service.configure(m_config);
  loadKeyFromKeychain();
  connect(
      &m_service, &WellFaciesService::modelsReady, this,
      [this](const QVector<WellFaciesModel> &models) {
        m_loading = false;
        m_modelError.clear();
        m_models = models;
        bool selected = false;
        for (const auto &m : models)
          if (m.id == m_modelId)
            selected = true;
        if (!selected)
          m_modelId = models.isEmpty() ? QString() : models.first().id;
        QVariantList rows;
        for (const auto &m : models) {
          rows.append(QVariantMap{
              {"id", m.id},
              {"selected", m.id == m_modelId},
              {"name", m.name + QStringLiteral(" · ") + m.version},
              {"requirements", tr("%1；曲线 %2；段、岩性；至少 %3 点")
                                   .arg(m.formationGroup, m.curves.join(", "))
                                   .arg(m.window)}});
        }
        emit modelsChanged(rows);
        updateInput();
      });
  connect(&m_service, &WellFaciesService::failed, this,
          [this](const QString &reason) {
            if (m_loading) {
              m_loading = false;
              m_modelError = reason;
            }
            m_busy = false;
            emit busyChanged(false);
            updateInput();
            emit statusChanged(reason);
          });
  connect(&m_service, &WellFaciesService::progress, this,
          [this](int pct, const QString &message) {
            emit statusChanged(tr("测井相预测 %1% · %2").arg(pct).arg(message));
          });
  connect(
      &m_service, &WellFaciesService::completed, this,
      [this](const WellFaciesResult &result) {
        m_busy = false;
        emit busyChanged(false);
        const QString path = cachePath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        const QByteArray bytes =
            QJsonDocument(result.response).toJson(QJsonDocument::Compact);
        const bool saved = file.open(QIODevice::WriteOnly) &&
                           file.write(bytes) == bytes.size() && file.commit();
        // 产品装配回写可维护矢量相属性；旧测试/无图层服务保留资产兼容路。
        const QString publishNote = publishLithoAsset(result);
        updateInput(false);
        emit resultReady(result);
        QString note = saved ? tr("结果已保存") : tr("结果缓存保存失败：%1")
                                                      .arg(file.errorString());
        if (!publishNote.isEmpty())
          note += tr("；地质属性未保存：%1").arg(publishNote);
        else if (m_catalog)
          note += tr("；地质成果已保存");
        emit statusChanged(tr("预测完成：%1 个深度点 · %2；%3")
                               .arg(m_input.rows.size())
                               .arg(result.modelName)
                               .arg(note));
      });
}
void WellFaciesWorkflow::loadKeyFromKeychain() {
  if (!WellFaciesKeyStore::available())
    return;
  // 旧版 JSON 里的明文密钥：迁入钥匙串，成功后改写 JSON 去掉密钥。
  if (!m_config.apiKey.isEmpty() &&
      !qEnvironmentVariableIsSet("PALEO_WELL_FACIES_API_KEY")) {
    const WellFaciesConfig legacy = m_config;
    WellFaciesKeyStore::write(this, legacy.apiKey,
                              [legacy](bool ok, const QString &) {
                                QString ignored;
                                if (ok && legacy.baseUrl.isValid())
                                  legacy.save(&ignored, false);
                              });
    return;
  }
  if (!m_config.apiKey.isEmpty())
    return;
  const int generation = m_configGeneration;
  WellFaciesKeyStore::read(
      this, [this, generation](bool found, const QByteArray &key,
                               const QString &error) {
        if (generation != m_configGeneration || !m_config.apiKey.isEmpty())
          return;
        if (!found) {
          if (!error.isEmpty())
            qWarning("well-facies: keychain read failed: %s", qPrintable(error));
          return;
        }
        m_config.apiKey = key;
        m_service.configure(m_config);
        updateInput();
      });
}
void WellFaciesWorkflow::configure(const WellFaciesConfig &config,
                                   bool persist) {
  QString error;
  const bool useKeychain = persist && WellFaciesKeyStore::available();
  if (persist && !config.save(&error, !useKeychain)) {
    emit statusChanged(error);
    return;
  }
  if (useKeychain)
    WellFaciesKeyStore::write(this, config.apiKey,
                              [this, config](bool ok, const QString &err) {
                                if (ok)
                                  return;
                                // 钥匙串写失败：回落文件（POSIX 0600）并如实告知。
                                QString fileError;
                                config.save(&fileError, true);
                                emit statusChanged(
                                    tr("API 密钥未能写入系统钥匙串（%1），已改存本机"
                                       "配置文件（仅当前用户可读写）")
                                        .arg(err));
                              });
  if (config.allowInsecureHttp &&
      config.baseUrl.scheme() == QLatin1String("http") &&
      !WellFaciesConfig::isLoopbackHost(config.baseUrl.host()))
    emit statusChanged(tr("警告：已允许不加密的 HTTP，API 密钥与井数据将明文传输"));
  ++m_configGeneration;
  cancel();
  m_config = config;
  m_service.configure(config);
  m_models.clear();
  m_modelId.clear();
  emit modelsChanged({});
  refreshModels();
}
void WellFaciesWorkflow::setCatalog(DataCatalog *catalog,
                                    const QString &projectDir) {
  m_catalog = catalog;
  m_projectDir = projectDir;
  // 未显式给目录时，在访问时根据当前 catalog 解析，避免换工程后仍写旧目录。
}

QString
WellFaciesWorkflow::publishLithoAsset(const WellFaciesResult &result) {
  if (m_layers) return publishAttributes(result);
  const QString dir = projectDir();
  if (!m_catalog || m_catalog->refusesWrites() || dir.isEmpty())
    return tr("工程 catalog 不可写或未绑定");
  // 井 id 解析：井名规范化后唯一命中才挂链接（§3 身份解析口径——不猜不并）。
  const QStringList wellIds = m_catalog->wellsMatchingName(m_data.wellName);
  if (wellIds.size() != 1)
    return tr("井名 %1 在 catalog 中未唯一解析（%2 个候选），未登记")
        .arg(m_data.wellName)
        .arg(wellIds.size());
  const QString wellId = wellIds.first();
  // 消费契约（与剖面 attachLithoSegments 严格同构）：
  // {schema:1, provenance:{source,modelName,modelVersion,jobId},
  //  intervals:[{wellId,top,base,litho}]} —— litho 为预测相中文自由词面。
  QJsonArray intervals;
  int dropped = 0;
  for (const WellComposite::TextInterval &iv : result.intervals) {
    const QString word = iv.text.trimmed();
    if (word.isEmpty() || !(iv.bottomDepth > iv.topDepth)) {
      ++dropped;
      continue;
    }
    intervals.append(QJsonObject{
        {QStringLiteral("wellId"), wellId},
        {QStringLiteral("top"), iv.topDepth},
        {QStringLiteral("base"), iv.bottomDepth},
        {QStringLiteral("litho"), word}});
  }
  if (intervals.isEmpty())
    return dropped > 0 ? tr("预测段全部无效（空词面/逆序），未登记")
                       : tr("预测结果无岩性段，未登记");
  const QJsonObject provenance{
      {QStringLiteral("source"), QStringLiteral("welllogfacies")},
      {QStringLiteral("modelName"), result.modelName},
      {QStringLiteral("modelVersion"), result.modelVersion},
      {QStringLiteral("jobId"), result.jobId}};
  const QJsonObject root{{QStringLiteral("schema"), 1},
                         {QStringLiteral("provenance"), provenance},
                         {QStringLiteral("intervals"), intervals}};
  DerivedAssetRegistrar registrar(m_catalog, dir);
  QString error;
  const QString displayName =
      tr("测井相解释岩性 %1").arg(m_data.wellName);
  const DerivedStaging stage = registrar.stage(
      QStringLiteral("well_litho_intervals"), displayName,
      QStringLiteral("well-litho-intervals.json"), &error);
  if (!stage.isValid())
    return error;
  const QByteArray bytes =
      QJsonDocument(root).toJson(QJsonDocument::Compact);
  QSaveFile file(stage.absolutePath);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(bytes) != bytes.size() || !file.commit()) {
    QFile::remove(stage.absolutePath);
    return tr("资产文件写入失败：%1").arg(file.errorString());
  }
  QVariantMap extra;
  extra.insert(QStringLiteral("source"),
               QStringLiteral("welllogfacies"));
  extra.insert(QStringLiteral("modelName"), result.modelName);
  extra.insert(QStringLiteral("modelVersion"), result.modelVersion);
  extra.insert(QStringLiteral("jobId"), result.jobId);
  extra.insert(QStringLiteral("droppedIntervals"), dropped);
  if (!registrar.commit(stage, {},
                        QStringLiteral("welllogfacies://%1")
                            .arg(result.jobId),
                        extra, &error)) {
    QFile::remove(stage.absolutePath);
    return error;
  }
  EntityAssetLink link;
  link.entityType = QStringLiteral("well");
  link.entityId = wellId;
  link.assetId = stage.assetId;
  link.role = QStringLiteral("interpretation");
  link.note = tr("测井相解释岩性（%1 %2）")
                  .arg(result.modelName, result.modelVersion);
  if (!m_catalog->addLink(link, &error))
    return tr("成果已登记，但井关联失败：%1").arg(error);
  return QString();
}

QString WellFaciesWorkflow::projectDir() const {
  return !m_projectDir.isEmpty() ? m_projectDir
      : m_catalog && m_catalog->isOpen() ? QDir::cleanPath(m_catalog->catalogPath() + "/../../..") : QString();
}
void WellFaciesWorkflow::setLayerService(QgisLayerService *layers) {
  m_layers = layers;
}
void WellFaciesWorkflow::refreshAttributes() {
  if (!m_catalog || !m_layers || !m_catalog->isOpen()) {
    emit attributeAvailabilityChanged(false, tr("请先打开工程并绑定图层服务")); return;
  }
  const auto ids = m_catalog->wellsMatchingName(m_sourceData.wellName);
  if (ids.size() != 1) {
    emit attributeAvailabilityChanged(false, tr("当前井名未在工程中唯一解析")); return;
  }
  emit attributeAvailabilityChanged(true, tr("打开工程矢量属性表；保存后更新井道与单因素输入"));
  m_data = m_sourceData;
  WellAttributeStore::apply(WellAttributeStore::rows(projectDir(), m_layers, false, ids.first()), &m_data);
  emit attributesReady(m_data);
  auto *layer = WellAttributeStore::open(m_catalog, projectDir(), m_layers, false, false);
  if (layer && layer != m_attributeLayer) {
    if (m_attributeLayer) disconnect(m_attributeLayer, nullptr, this, nullptr);
    m_attributeLayer = layer;
    connect(layer, &QgsVectorLayer::editingStarted, this, [this] { updateInput(false); });
    connect(layer, &QgsVectorLayer::editingStopped, this, [this] { updateInput(false); });
    connect(layer, &QgsVectorLayer::afterCommitChanges, this, [this] {
      QTimer::singleShot(0, this, [this] {
        refreshAttributes();
        if (m_busy) {
          for (const auto &model : m_models)
            if (model.id == m_modelId && prepareWellFaciesInput(m_data, model).rows != m_input.rows) {
              cancel(); return;
            }
        } else updateInput(false);
      });
    });
  }
}
void WellFaciesWorkflow::requestAttributeTable(bool factors) {
  if (factors) { emit factorMaintenanceRequested(); return; }
  QString error;
  auto *layer = WellAttributeStore::open(m_catalog, projectDir(), m_layers, factors, true, &error);
  if (!layer) { emit statusChanged(error.isEmpty() ? tr("请先打开工程并绑定当前井") : error); return; }
  emit attributeTableRequested(factors ? WellAttributeStore::factorLayerId() : WellAttributeStore::intervalLayerId());
}
QString WellFaciesWorkflow::publishAttributes(const WellFaciesResult &result) {
  if (!m_catalog) return tr("工程 catalog 未绑定");
  const auto ids = m_catalog->wellsMatchingName(m_data.wellName);
  if (ids.size() != 1) return tr("井名未在工程中唯一解析，未更新属性");
  QVariantList intervals;
  for (const auto &iv : result.intervals) {
    if (iv.text.trimmed().isEmpty()) return tr("预测相为空，未更新属性");
    intervals << QVariantMap{{"well_id", ids.first()}, {"top_md", iv.topDepth}, {"base_md", iv.bottomDepth},
      {"facies", iv.text}, {"predicted_facies", iv.text}, {"facies_code", QVariant()}, {"sub_facies", QString()}, {"micro_facies", QString()}, {"facies_pattern", QString()}, {"model", result.modelName + " " + result.modelVersion}, {"job_id", result.jobId}};
  }
  QString error;
  if (intervals.isEmpty()) return tr("预测没有有效井段，未更新属性");
  if (!WellAttributeStore::mergeIntervals(m_catalog, projectDir(), m_layers, intervals, &error)) return error;
  refreshAttributes();
  return {};
}

void WellFaciesWorkflow::setData(
    const WellComposite::ComprehensiveWellData &data) {
  if (m_busy)
    cancel();
  m_sourceData = data;
  m_data = data;
  if (m_layers && m_catalog && !m_catalog->refusesWrites()) {
    const auto ids = m_catalog->wellsMatchingName(data.wellName);
    QString error;
    if (ids.size() == 1 && !WellAttributeStore::seed(m_catalog, projectDir(), m_layers, ids.first(), data, &error))
      emit statusChanged(error);
  }
  emit resultCleared();
  refreshAttributes();
  updateInput();
  if (m_models.isEmpty() && !m_loading && m_modelError.isEmpty())
    refreshModels();
}
void WellFaciesWorkflow::selectModel(const QString &id) {
  if (m_busy || id == m_modelId)
    return;
  m_modelId = id;
  emit resultCleared();
  updateInput();
}
void WellFaciesWorkflow::refreshModels() {
  if (m_busy)
    return;
  m_loading = true;
  m_modelError.clear();
  m_models.clear();
  emit resultCleared();
  updateInput();
  m_service.fetchModels();
}
QString WellFaciesWorkflow::cachePath() const {
  const QByteArray bytes =
      QJsonDocument(QJsonObject{{"service", m_config.baseUrl.toString()},
                                {"modelId", m_modelId},
                                {"wellName", m_data.wellName},
                                {"rows", m_input.rows}})
          .toJson(QJsonDocument::Compact);
  const QString hash = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/paleo/well-facies/") + hash + QStringLiteral(".json");
}
void WellFaciesWorkflow::updateInput(bool restoreCache) {
  if (m_busy) { emit availabilityChanged(false, tr("测井相预测正在执行")); return; }
  m_input = {};
  QString reason;
  if (m_attributeLayer && m_attributeLayer->isEditable())
    reason = tr("测井属性表正在编辑，请先保存或放弃编辑");
  else if (m_config.apiKey.isEmpty())
    reason = tr("请在「预测服务」配置 API 密钥");
  else if (m_loading)
    reason = tr("正在获取模型输入要求");
  else if (!m_modelError.isEmpty())
    reason = m_modelError;
  else {
    for (const auto &m : m_models)
      if (m.id == m_modelId) {
        m_input = prepareWellFaciesInput(m_data, m);
        break;
      }
    reason = m_input.ready() ? QString()
             : m_input.reason.isEmpty()
                 ? tr("没有可调用的测井相模型，请刷新模型")
                 : m_input.reason;
  }
  emit availabilityChanged(reason.isEmpty(), reason);
  if (!reason.isEmpty() || !restoreCache)
    return;
  QFile file(cachePath());
  if (file.open(QIODevice::ReadOnly)) {
    WellFaciesResult result;
    QString error;
    if (parseWellFaciesResult(QJsonDocument::fromJson(file.readAll()).object(),
                              m_data.wellName, m_input, &result, &error))
      emit resultReady(result);
  }
}
void WellFaciesWorkflow::run() {
  if (m_busy || m_loading || !m_input.ready() || (m_attributeLayer && m_attributeLayer->isEditable()))
    return;
  for (const auto &m : m_models)
    if (m.id == m_modelId) {
      m_busy = true;
      emit busyChanged(true);
      emit availabilityChanged(false, tr("测井相预测正在执行"));
      emit statusChanged(
          tr("正在提交 %1 个深度点，等待网络推理…").arg(m_input.rows.size()));
      m_service.predict(m, m_data.wellName, m_input);
      return;
    }
}
void WellFaciesWorkflow::cancel() {
  if (m_loading) {
    m_loading = false;
    m_modelError = tr("模型加载已停止，请刷新模型");
  }
  m_service.cancel();
  const bool wasBusy = m_busy;
  m_busy = false;
  emit busyChanged(false);
  updateInput();
  if (wasBusy)
    emit statusChanged(tr("已停止本地等待；已受理任务仍可能在服务端执行"));
}
