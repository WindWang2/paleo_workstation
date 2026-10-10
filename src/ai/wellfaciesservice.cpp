// 层：功能
#include "wellfaciesservice.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

QString WellFaciesConfig::path() {
  // PALEO_WELL_FACIES_CONFIG：配置文件全路径覆盖（同 _URL/_API_KEY 一族）。
  // Windows 的 GenericConfigLocation 是 known folder（%LOCALAPPDATA%），不看
  // XDG_CONFIG_HOME——测试/便携场景只能经此把文件挪离用户真实配置（方向 81）。
  const QString overridePath =
      qEnvironmentVariable("PALEO_WELL_FACIES_CONFIG").trimmed();
  if (!overridePath.isEmpty())
    return overridePath;
  return QStandardPaths::writableLocation(
             QStandardPaths::GenericConfigLocation) +
         QStringLiteral("/paleo/well-facies.json");
}
bool WellFaciesConfig::isLoopbackHost(const QString &host) {
  const QString h = host.trimmed().toLower();
  if (h == QLatin1String("localhost"))
    return true;
  QHostAddress address;
  if (!address.setAddress(h))
    return false;
  return address.isLoopback();
}
QString WellFaciesConfig::validateUrl(const QUrl &url, bool allowInsecureHttp) {
  if (url.isEmpty())
    return QObject::tr("尚未配置测井相预测服务地址");
  if (!url.isValid() || url.host().isEmpty() ||
      (url.scheme() != QLatin1String("http") &&
       url.scheme() != QLatin1String("https")) ||
      !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
    return QObject::tr("测井相预测服务地址无效");
  if (url.scheme() == QLatin1String("http") && !allowInsecureHttp &&
      !isLoopbackHost(url.host()))
    return QObject::tr("测井相预测服务必须使用 https://（http 会明文传输 API "
                       "密钥与井数据）；确需内网 http 请在「预测服务」勾选"
                       "「允许不加密的 HTTP」");
  return {};
}
WellFaciesConfig WellFaciesConfig::load() {
  WellFaciesConfig c;
  QFile f(path());
  if (f.open(QIODevice::ReadOnly)) {
    const auto j = QJsonDocument::fromJson(f.readAll()).object();
    if (!j.value("baseUrl").toString().isEmpty())
      c.baseUrl = QUrl(j.value("baseUrl").toString());
    c.apiKey = j.value("apiKey").toString().toUtf8();
    c.allowInsecureHttp = j.value("allowInsecureHttp").toBool(false);
    // 旧配置只存了 http 地址和密钥，没有「允许不加密」开关。那是用户已经
    // 选定的服务，按明文传输继续用，否则「预测相」会一直停在地址校验。
    if (!j.contains(QLatin1String("allowInsecureHttp"))
        && c.baseUrl.scheme().compare(QLatin1String("http"), Qt::CaseInsensitive) == 0
        && !c.apiKey.trimmed().isEmpty())
      c.allowInsecureHttp = true;
  }
  if (qEnvironmentVariableIsSet("PALEO_WELL_FACIES_URL"))
    c.baseUrl = QUrl(qEnvironmentVariable("PALEO_WELL_FACIES_URL"));
  if (qEnvironmentVariableIsSet("PALEO_WELL_FACIES_API_KEY"))
    c.apiKey = qgetenv("PALEO_WELL_FACIES_API_KEY");
  if (qEnvironmentVariable("PALEO_WELL_FACIES_ALLOW_INSECURE_HTTP") ==
      QLatin1String("1"))
    c.allowInsecureHttp = true;
  return c;
}
bool WellFaciesConfig::save(QString *error, bool writeKeyToFile) const {
  const QString urlError = validateUrl(baseUrl, allowInsecureHttp);
  if (!urlError.isEmpty() || apiKey.trimmed().isEmpty() ||
      apiKey.contains('\n') || apiKey.contains('\r')) {
    if (error)
      *error = urlError.isEmpty()
                   ? QObject::tr("请输入有效的服务地址和 API 密钥")
                   : urlError;
    return false;
  }
  QDir().mkpath(QFileInfo(path()).absolutePath());
  QSaveFile f(path());
  if (f.open(QIODevice::WriteOnly)) {
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QJsonObject json{{"baseUrl", baseUrl.toString()}};
    if (allowInsecureHttp)
      json.insert("allowInsecureHttp", true);
    if (writeKeyToFile)
      json.insert("apiKey", QString::fromUtf8(apiKey));
    const QByteArray bytes = QJsonDocument(json).toJson();
    if (f.write(bytes) == bytes.size() && f.commit())
      return true;
  }
  if (error)
    *error = f.errorString();
  return false;
}
WellFaciesService::WellFaciesService(QObject *parent) : QObject(parent) {
  m_poll.setSingleShot(true);
  connect(&m_poll, &QTimer::timeout, this, [this] {
    if (m_elapsed.elapsed() > 15 * 60 * 1000)
      return fail(
          tr("推理等待超过 15 分钟；任务可能仍在服务端执行，任务 ID：%1")
              .arg(m_jobId));
    request(QStringLiteral("predictions/") +
                QString::fromLatin1(QUrl::toPercentEncoding(m_jobId)),
            {}, false, false);
  });
}
void WellFaciesService::configure(const WellFaciesConfig &config) {
  cancel();
  m_config = config;
  m_jobId.clear();
}
void WellFaciesService::cancel() {
  ++m_generation;
  m_poll.stop();
  if (m_reply) {
    auto reply = m_reply;
    m_reply = nullptr;
    reply->abort();
    reply->deleteLater();
  }
}
void WellFaciesService::fail(const QString &reason) {
  cancel();
  emit failed(reason);
}
void WellFaciesService::fetchModels() {
  cancel();
  request(QStringLiteral("models"), {}, false, true);
}
void WellFaciesService::predict(const WellFaciesModel &model,
                                const QString &wellName,
                                const WellFaciesInput &input) {
  const bool resume = !m_jobId.isEmpty() && model.id == m_modelId &&
                      wellName == m_wellName && input.rows == m_input.rows;
  cancel();
  m_elapsed.start();
  if (resume) {
    emit progress(0, tr("继续查询已受理的任务 %1").arg(m_jobId));
    request(QStringLiteral("predictions/") +
                QString::fromLatin1(QUrl::toPercentEncoding(m_jobId)),
            {}, false, false);
    return;
  }
  m_jobId.clear();
  m_modelId = model.id;
  m_input = input;
  m_wellName = wellName;
  if (!input.ready())
    return fail(input.reason);
  const QJsonObject well{{"wellName", wellName}, {"rows", input.rows}};
  request(QStringLiteral("predict"),
          {{"modelVersionId", model.id},
           {"waitTimeoutSeconds", 30},
           {"wells", QJsonArray{well}}},
          true, false);
}
void WellFaciesService::request(const QString &path, const QJsonObject &body,
                                bool post, bool models) {
  if (m_config.apiKey.trimmed().isEmpty() || m_config.apiKey.contains('\n') ||
      m_config.apiKey.contains('\r'))
    return fail(tr("尚未配置测井相预测 API 密钥"));
  QUrl url = m_config.baseUrl;
  const QString urlError =
      WellFaciesConfig::validateUrl(url, m_config.allowInsecureHttp);
  if (!urlError.isEmpty())
    return fail(urlError);
  QString basePath = url.path();
  while (basePath.endsWith('/'))
    basePath.chop(1);
  url.setPath(basePath + '/' + path);
  QNetworkRequest req(url);
  req.setRawHeader("X-API-Key", m_config.apiKey);
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("application/json"));
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                   QNetworkRequest::ManualRedirectPolicy);
  req.setTransferTimeout(45000);
  auto *reply =
      post ? m_network.post(req,
                            QJsonDocument(body).toJson(QJsonDocument::Compact))
           : m_network.get(req);
  m_reply = reply;
  const int generation = m_generation;
  // 响应体上限（#133）：声明或实收超过上限即中止，finished 里判失败。
  connect(reply, &QNetworkReply::downloadProgress, reply,
          [reply](qint64 received, qint64 total) {
            if (received > WellFaciesConfig::kMaxResponseBytes ||
                total > WellFaciesConfig::kMaxResponseBytes) {
              reply->setProperty("paleoTooLarge", true);
              reply->abort();
            }
          });
  connect(
      reply, &QNetworkReply::finished, this, [this, reply, generation, models] {
        if (generation != m_generation) {
          reply->deleteLater();
          return;
        }
        if (reply->property("paleoTooLarge").toBool()) {
          reply->deleteLater();
          m_reply = nullptr;
          return fail(tr("测井相服务响应超过 %1 MiB 上限，已中止")
                          .arg(WellFaciesConfig::kMaxResponseBytes / (1024 * 1024)));
        }
        const auto bytes = reply->readAll();
        const int http =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply->error();
        const QString networkMessage = reply->errorString();
        reply->deleteLater();
        m_reply = nullptr;
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(bytes, &parse);
        const auto json = document.object();
        if (http != 200 && http != 202) {
          QString detail = json.value("error").toString();
          if (detail.isEmpty() && json.value("error").isObject())
            detail = json.value("error").toObject().value("message").toString();
          if (detail.isEmpty())
            detail = json.value("message").toString(networkMessage);
          if (!json.value("code").toString().isEmpty())
            detail.prepend(json.value("code").toString() +
                           QStringLiteral(": "));
          detail.replace(QString::fromUtf8(m_config.apiKey),
                         QStringLiteral("[密钥已隐藏]"));
          return fail(
              tr("测井相服务请求失败（HTTP %1）：%2").arg(http).arg(detail));
        }
        if (networkError != QNetworkReply::NoError ||
            parse.error != QJsonParseError::NoError || !document.isObject())
          return fail(tr("测井相服务返回无效 JSON 或网络连接中断"));
        if (!models)
          return handlePrediction(json);
        if (!json.value("models").isArray())
          return fail(tr("模型列表响应缺少 models"));
        QVector<WellFaciesModel> list;
        for (const auto &v : json.value("models").toArray()) {
          const auto j = v.toObject(),
                     schema = j.value("inputSchema").toObject();
          WellFaciesModel m;
          m.id = j.value("id").toString();
          m.name = j.value("name").toString();
          m.version = j.value("version").toString();
          m.formationGroup = schema.value("formationGroup").toString();
          m.window = schema.value("window").toInt(1);
          for (const auto &c : schema.value("curves").toArray())
            m.curves.append(c.toString());
          for (const auto &c : schema.value("labels").toArray())
            m.labels.append(c.toString());
          bool categoriesSupported = true;
          for (const auto &c : schema.value("categoricalFields").toArray())
            if (!QStringList{QStringLiteral("段"), QStringLiteral("岩性")}
                     .contains(c.toString()))
              categoriesSupported = false;
          QSet<QString> curveNames;
          for (const auto &c : m.curves)
            if (!c.isEmpty())
              curveNames.insert(c);
          if (!m.id.isEmpty() && m.window > 0 && !m.curves.isEmpty() &&
              curveNames.size() == m.curves.size() && categoriesSupported)
            list.append(m);
        }
        emit modelsReady(list);
      });
}
void WellFaciesService::handlePrediction(const QJsonObject &json) {
  const QString status = json.value("status").toString();
  if (status == "completed") {
    const QString modelId =
        json.value("model").toObject().value("id").toString();
    if (!modelId.isEmpty() && modelId != m_modelId)
      return fail(tr("预测结果的模型与所选模型不一致"));
    WellFaciesResult result;
    QString error;
    if (!parseWellFaciesResult(json, m_wellName, m_input, &result, &error))
      return fail(error);
    m_jobId.clear();
    emit completed(result);
    return;
  }
  if (status == "failed" || status == "canceled") {
    m_jobId.clear();
    QString error = json.value("error").toString();
    if (error.isEmpty())
      error = QString::fromUtf8(QJsonDocument(json.value("error").toObject())
                                    .toJson(QJsonDocument::Compact));
    error.replace(QString::fromUtf8(m_config.apiKey),
                  QStringLiteral("[密钥已隐藏]"));
    return fail(
        tr("测井相推理%1：%2")
            .arg(status == "failed" ? tr("失败") : tr("已取消"), error));
  }
  if (!QStringList{"queued", "preprocessing", "predicting"}.contains(status))
    return fail(tr("未知推理状态：%1").arg(status));
  const QString id = json.value("jobId").toString();
  if (!id.isEmpty()) {
    if (!m_jobId.isEmpty() && m_jobId != id)
      return fail(tr("轮询返回了不同的推理任务"));
    m_jobId = id;
  }
  if (m_jobId.isEmpty())
    return fail(tr("推理已受理，但响应缺少任务 ID"));
  emit progress(
      qBound(0, json.value("progress").toInt(), 100),
      json.value("latestMessage")
          .toString(json.value("message").toString(tr("等待推理完成"))));
  m_poll.start(qBound(100, json.value("pollAfterMs").toInt(2000), 30000));
}
