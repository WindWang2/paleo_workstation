// 层：功能
#include "wellfaciesworkflow.h"
#include "ai/wellfacieskeystore.h"
#include "../catalog/datacatalog.h"
#include "derivedassets.h"
#include "../services/paleotaskservice.h"
#include "../algorithms/faciesmapping/formationdominance.h"
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
#include <QSet>
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
            if (m_busy && m_runIndex >= 0) {
              onGroupFailed(reason);
              return;
            }
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
            if (m_runIndex < 0 || m_runIndex >= m_runs.size())
              return;
            if (const auto task = m_runTasks.value(m_runIndex)) {
              task->reportBytes(pct, 100);
              task->reportDetail(message);
            }
            emit statusChanged(tr("测井相预测 %1（%2/%3）%4% · %5")
                                   .arg(runLabel(m_runs[m_runIndex]))
                                   .arg(m_runIndex + 1)
                                   .arg(m_runs.size())
                                   .arg(pct)
                                   .arg(message));
          });
  connect(&m_service, &WellFaciesService::completed, this,
          [this](const WellFaciesResult &part) { onGroupCompleted(part); });
}
WellFaciesWorkflow::~WellFaciesWorkflow() {
  if (m_busy)
    abortRun();
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
  const QString &wellId = wellIds.first();
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
void WellFaciesWorkflow::setTaskService(PaleoTaskService *tasks) {
  m_taskSvc = tasks;
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
          for (int i = qMax(m_runIndex, 0); i < m_runs.size(); ++i)
            if (prepareWellFaciesInput(m_data, m_runs[i].model).rows != m_runs[i].input.rows) {
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
WellFaciesResult WellFaciesWorkflow::withDominance(WellFaciesResult result) const {
  QVector<FaciesSpan> spans;
  for (const auto &iv : result.intervals)
    spans.append({iv.topDepth, iv.bottomDepth, iv.text});
  result.dominant = paleo::faciesmapping::dominantFaciesByFormation(m_data.formationIntervals, spans);
  return result;
}
void WellFaciesWorkflow::requestFormationTable() {
  QString error;
  if (!WellAttributeStore::open(m_catalog, projectDir(), m_layers, WellAttributeStore::Table::Formations, true, &error)) {
    emit statusChanged(error.isEmpty() ? tr("请先打开工程并绑定当前井") : error);
    return;
  }
  emit attributeTableRequested(WellAttributeStore::formationLayerId());
}
QString WellFaciesWorkflow::publishAttributes(const WellFaciesResult &result) {
  if (!m_catalog) return tr("工程 catalog 未绑定");
  const auto ids = m_catalog->wellsMatchingName(m_data.wellName);
  if (ids.size() != 1) return tr("井名未在工程中唯一解析，未更新属性");
  QVariantList intervals;
  // 与 publishLithoAsset 同口径逐段过滤（空词面/逆序段丢弃计数），不再
  // 「任一段无效即整批放弃」；逆序段进 mergeIntervals 会被 span() 判死并
  // 报笼统错误，用户无从定位。
  int dropped = 0;
  for (const auto &iv : result.intervals) {
    if (iv.text.trimmed().isEmpty() || !(iv.bottomDepth > iv.topDepth)) {
      ++dropped;
      continue;
    }
    intervals << QVariantMap{{"well_id", ids.first()}, {"top_md", iv.topDepth}, {"base_md", iv.bottomDepth},
      {"facies", iv.text}, {"predicted_facies", iv.text}, {"facies_code", QVariant()}, {"sub_facies", QString()}, {"micro_facies", QString()}, {"facies_pattern", QString()}, {"model", result.modelName + " " + result.modelVersion}, {"job_id", result.jobId}};
  }
  QString error;
  if (intervals.isEmpty())
    return dropped > 0 ? tr("预测段全部无效（空词面/逆序），未更新属性")
                       : tr("预测没有有效井段，未更新属性");
  if (!WellAttributeStore::mergeIntervals(m_catalog, projectDir(), m_layers, intervals, &error)) return error;
  // 地层优势相：组、段各一行（同井同名只留最上一个，名字不能重复进矢量表）。
  QVariantList formations;
  QSet<QString> seen;
  const WellFaciesResult ranked = result.dominant.isEmpty() ? withDominance(result) : result;
  for (const auto &d : ranked.dominant) {
    if (seen.contains(d.formation)) continue;
    seen.insert(d.formation);
    formations << QVariantMap{{"well_id", ids.first()}, {"formation", d.formation}, {"unit_type", d.unitType},
      {"top_md", d.top}, {"base_md", d.bottom}, {"thickness_md", d.thickness},
      {"dominant_facies", d.dominant}, {"predicted_dominant", d.dominant}, {"facies_code", QVariant()},
      {"dominant_thickness_md", d.dominantThickness}, {"dominant_ratio", d.ratio}, {"coverage", d.coverage},
      {"tie", d.tie}, {"facies_composition", d.composition}, {"source", QStringLiteral("welllogfacies")},
      {"model", result.modelName + " " + result.modelVersion}, {"job_id", result.jobId}};
  }
  const bool formationsSaved = WellAttributeStore::mergeFormations(m_catalog, projectDir(), m_layers, formations, &error);
  refreshAttributes();
  return formationsSaved ? QString() : tr("井段属性已更新，地层优势相未更新：%1").arg(error);
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
QString WellFaciesWorkflow::cachePath(const WellFaciesRun &run) const {
  const QByteArray bytes =
      QJsonDocument(QJsonObject{{"service", m_config.baseUrl.toString()},
                                {"modelId", run.model.id},
                                {"wellName", m_data.wellName},
                                {"rows", run.input.rows}})
          .toJson(QJsonDocument::Compact);
  const QString hash = QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/paleo/well-facies/") + hash + QStringLiteral(".json");
}
QString WellFaciesWorkflow::runLabel(const WellFaciesRun &run) const {
  return run.group.isEmpty() ? tr("整井") : run.group;
}
QString WellFaciesWorkflow::planSummary() const {
  QStringList parts;
  for (const auto &run : m_plan.runs)
    parts << tr("%1 %2 点（模型 %3 %4）")
                 .arg(runLabel(run))
                 .arg(run.input.rows.size())
                 .arg(run.model.name, run.model.version);
  QString text = parts.join(QStringLiteral("；"));
  if (!text.isEmpty() && !m_plan.skipped.isEmpty())
    text += tr("；跳过：%1").arg(m_plan.skipped.join(QStringLiteral("、")));
  return text;
}
void WellFaciesWorkflow::updateInput(bool restoreCache) {
  if (m_busy) { emit availabilityChanged(false, tr("测井相预测正在执行")); return; }
  m_plan = {};
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
    m_plan = planWellFaciesRuns(m_data, m_models, m_modelId);
    reason = m_plan.ready() ? QString()
             : m_plan.reason.isEmpty()
                 ? tr("没有可调用的测井相模型，请刷新模型")
                 : m_plan.reason;
  }
  emit planChanged(reason.isEmpty() ? planSummary() : QString());
  emit availabilityChanged(reason.isEmpty(), reason);
  if (!reason.isEmpty() || !restoreCache)
    return;
  QVector<WellFaciesResult> cached;
  for (const auto &run : m_plan.runs) {
    QFile file(cachePath(run));
    if (!file.open(QIODevice::ReadOnly))
      continue;
    WellFaciesResult result;
    QString error;
    if (parseWellFaciesResult(QJsonDocument::fromJson(file.readAll()).object(),
                              m_data.wellName, run.input, &result, &error)) {
      result.group = run.group;
      cached.append(result);
    }
  }
  if (!cached.isEmpty())
    emit resultReady(withDominance(mergeWellFaciesResults(cached)));
}
void WellFaciesWorkflow::run() {
  if (m_busy || m_loading || !m_plan.ready() || (m_attributeLayer && m_attributeLayer->isEditable()))
    return;
  m_busy = true;
  m_runs = m_plan.runs;
  m_runTasks.clear();
  m_runSkip = QVector<bool>(m_runs.size(), false);
  m_runSession = m_taskSvc ? m_taskSvc->session() : 0;
  m_runResults.clear();
  m_runFailures.clear();
  m_runCancelled.clear();
  m_runCacheError.clear();
  m_runIndex = -1;
  for (const auto &run : m_runs) {
    PaleoTask *task = nullptr;
    if (m_taskSvc) {
      task = m_taskSvc->beginExternal(
          tr("测井相预测 · %1 · %2").arg(m_data.wellName, runLabel(run)));
      task->reportDetail(tr("排队等待"));
      connect(task, &PaleoTask::cancelAsked, this,
              [this, task] { onTaskCancelAsked(task); });
    }
    m_runTasks.append(task);
  }
  emit busyChanged(true);
  emit availabilityChanged(false, tr("测井相预测正在执行"));
  startNextRun();
}
void WellFaciesWorkflow::startNextRun() {
  while (++m_runIndex < m_runs.size()) {
    const auto task = m_runTasks.value(m_runIndex);
    const WellFaciesRun &run = m_runs[m_runIndex];
    if (m_runSkip.value(m_runIndex))
      continue;
    if (task && task->cancelRequested()) {
      m_runCancelled << runLabel(run);
      finishTask(m_runIndex);
      continue;
    }
    if (task)
      task->reportDetail(
          tr("已提交 %1 个深度点，等待网络推理").arg(run.input.rows.size()));
    emit statusChanged(tr("正在预测 %1（%2/%3）：提交 %4 个深度点，等待网络推理…")
                           .arg(runLabel(run))
                           .arg(m_runIndex + 1)
                           .arg(m_runs.size())
                           .arg(run.input.rows.size()));
    m_service.predict(run.model, m_data.wellName, run.input);
    return;
  }
  finishRun();
}
void WellFaciesWorkflow::onGroupCompleted(const WellFaciesResult &part) {
  if (!m_busy || m_runIndex < 0 || m_runIndex >= m_runs.size())
    return;
  const WellFaciesRun &run = m_runs[m_runIndex];
  WellFaciesResult result = part;
  result.group = run.group;
  const QString path = cachePath(run);
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile file(path);
  const QByteArray bytes =
      QJsonDocument(result.response).toJson(QJsonDocument::Compact);
  if (!(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
        file.commit()) &&
      m_runCacheError.isEmpty())
    m_runCacheError = file.errorString();
  m_runResults.append(result);
  finishTask(m_runIndex);
  emit resultReady(withDominance(mergeWellFaciesResults(m_runResults)));
  if (m_runIndex + 1 < m_runs.size())
    emit statusChanged(tr("%1 已返回（%2/%3）：%4 个深度点；继续预测下一个地层…")
                           .arg(runLabel(run))
                           .arg(m_runIndex + 1)
                           .arg(m_runs.size())
                           .arg(run.input.rows.size()));
  startNextRun();
}
void WellFaciesWorkflow::onGroupFailed(const QString &reason) {
  m_runFailures << tr("%1（%2）").arg(runLabel(m_runs[m_runIndex]), reason);
  finishTask(m_runIndex, reason);
  startNextRun();
}
void WellFaciesWorkflow::onTaskCancelAsked(PaleoTask *task) {
  // 延后一拍：会话切换/关停会连续取消全部任务，等标记都落下再处理，避免
  // 为已被取消的后续地层白发一次请求。
  QMetaObject::invokeMethod(
      this,
      [this, guard = QPointer<PaleoTask>(task)] {
        const int index = guard ? int(m_runTasks.indexOf(guard)) : -1;
        if (!m_busy || index < 0 || index < m_runIndex)
          return;
        m_runCancelled << runLabel(m_runs[index]);
        finishTask(index);
        if (index > m_runIndex) {
          m_runSkip[index] = true; // 排队中：立即结束任务行，轮到时跳过
          return;
        }
        m_service.cancel(); // 只停本地等待；服务端已受理任务继续执行
        startNextRun();
      },
      Qt::QueuedConnection);
}
void WellFaciesWorkflow::finishTask(int index, const QString &error) {
  if (index < 0 || index >= m_runTasks.size())
    return;
  if (PaleoTask *task = m_runTasks[index].data()) {
    disconnect(task, &PaleoTask::cancelAsked, this, nullptr);
    task->finishExternal(error);
  }
  m_runTasks[index].clear();
}
void WellFaciesWorkflow::abortRun() {
  for (int i = qMax(m_runIndex, 0); i < m_runTasks.size(); ++i)
    if (PaleoTask *task = m_runTasks[i].data()) {
      disconnect(task, &PaleoTask::cancelAsked, this, nullptr);
      task->requestCancel();
      task->finishExternal();
      m_runTasks[i].clear();
    }
}
void WellFaciesWorkflow::finishRun() {
  const QVector<WellFaciesResult> results = m_runResults;
  const QStringList failures = m_runFailures, cancelled = m_runCancelled;
  const QStringList skipped = m_plan.skipped;
  const QString cacheError = m_runCacheError;
  m_busy = false;
  m_runs.clear();
  m_runTasks.clear();
  m_runSkip.clear();
  m_runIndex = -1;
  emit busyChanged(false);
  const WellFaciesResult merged = withDominance(mergeWellFaciesResults(results));
  QString note;
  if (!results.isEmpty()) {
    // 产品装配回写可维护矢量相属性；旧测试/无图层服务保留资产兼容路。
    const bool stale = m_taskSvc && m_taskSvc->session() != m_runSession;
    const QString publishNote =
        stale ? tr("工程已切换，结果未写入属性") : publishLithoAsset(merged);
    note = cacheError.isEmpty() ? tr("结果已保存")
                                : tr("结果缓存保存失败：%1").arg(cacheError);
    if (!publishNote.isEmpty())
      note += tr("；地质属性未保存：%1").arg(publishNote);
    else if (m_catalog)
      note += tr("；地质成果已保存");
  }
  updateInput(false);
  if (results.isEmpty()) {
    emit statusChanged(!failures.isEmpty()
                           ? failures.join(QStringLiteral("；"))
                           : tr("已取消：%1").arg(cancelled.join(QStringLiteral("、"))));
    return;
  }
  qsizetype points = 0;
  QStringList groups;
  for (const auto &result : results) {
    points += result.confidence.depths.size();
    if (!result.group.isEmpty())
      groups << result.group;
  }
  QString status =
      results.size() == 1
          ? tr("预测完成：%1 个深度点 · %2；%3").arg(points).arg(merged.modelName, note)
          : tr("预测完成：%1 个地层（%2）· %3 个深度点 · %4；%5")
                .arg(results.size())
                .arg(groups.join(QStringLiteral("、")))
                .arg(points)
                .arg(merged.modelName, note);
  if (!failures.isEmpty())
    status += tr("；失败：%1").arg(failures.join(QStringLiteral("；")));
  if (!cancelled.isEmpty())
    status += tr("；已取消：%1").arg(cancelled.join(QStringLiteral("、")));
  if (!skipped.isEmpty())
    status += tr("；跳过：%1").arg(skipped.join(QStringLiteral("、")));
  emit statusChanged(status);
}
void WellFaciesWorkflow::cancel() {
  if (m_loading) {
    m_loading = false;
    m_modelError = tr("模型加载已停止，请刷新模型");
  }
  m_service.cancel();
  const bool wasBusy = m_busy;
  if (wasBusy)
    abortRun();
  m_busy = false;
  m_runs.clear();
  m_runTasks.clear();
  m_runSkip.clear();
  m_runIndex = -1;
  emit busyChanged(false);
  updateInput();
  if (wasBusy)
    emit statusChanged(tr("已停止本地等待；已受理任务仍可能在服务端执行"));
}
