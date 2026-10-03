// 层：功能
#include "wellfaciesworkflow.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

WellFaciesWorkflow::WellFaciesWorkflow(QObject *parent) : QObject(parent) {
  m_config = WellFaciesConfig::load();
  m_service.configure(m_config);
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
        updateInput(false);
        emit resultReady(result);
        emit statusChanged(saved ? tr("预测完成：%1 个深度点 · %2；结果已保存")
                                       .arg(m_input.rows.size())
                                       .arg(result.modelName)
                                 : tr("预测完成，但结果缓存保存失败：%1")
                                       .arg(file.errorString()));
      });
}
void WellFaciesWorkflow::configure(const WellFaciesConfig &config,
                                   bool persist) {
  QString error;
  if (persist && !config.save(&error)) {
    emit statusChanged(error);
    return;
  }
  cancel();
  m_config = config;
  m_service.configure(config);
  m_models.clear();
  m_modelId.clear();
  emit modelsChanged({});
  refreshModels();
}
void WellFaciesWorkflow::setData(
    const WellComposite::ComprehensiveWellData &data) {
  if (m_busy)
    cancel();
  m_data = data;
  emit resultCleared();
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
  m_input = {};
  QString reason;
  if (m_busy)
    reason = tr("测井相预测正在执行");
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
  if (m_busy || m_loading || !m_input.ready())
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
