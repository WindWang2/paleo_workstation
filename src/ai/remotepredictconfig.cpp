// 层：功能
#include "remotepredictconfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSaveFile>
#include <QStandardPaths>

// 环境变量覆盖口径照 WellFaciesConfig（PALEO_…_URL / 开关 / 模型）：本机构建
// 与 CI 都不想在用户配置目录里留下痕迹，测试也走注入路径（fromParts）。
namespace {
const char kEnvUrl[] = "PALEO_REMOTE_PREDICT_URL";
const char kEnvEnabled[] = "PALEO_REMOTE_PREDICT_ENABLED";
const char kEnvModel[] = "PALEO_REMOTE_PREDICT_MODEL";
} // namespace

QString RemotePredictConfig::path() {
  return QStandardPaths::writableLocation(
           QStandardPaths::GenericConfigLocation) +
         QStringLiteral("/paleo/remote-predict.json");
}

bool RemotePredictConfig::isLoopbackHost(const QString &host) {
  const QString h = host.trimmed().toLower();
  if (h == QLatin1String("localhost"))
    return true;
  QHostAddress address;
  if (!address.setAddress(h))
    return false;
  return address.isLoopback();
}

QString RemotePredictConfig::validate() const {
  if (baseUrl.isEmpty())
    return QObject::tr("远端预测未配置，走本地引擎");
  // 路径必须显式拒绝：路由侧 resolved("/health") 的前导斜杠是绝对路径
  // 引用，会替换 base 路径——带路径的配置此前静默打到错误端点（404）。
  const QString basePath = baseUrl.path();
  if (!baseUrl.isValid() || baseUrl.host().isEmpty() ||
      (baseUrl.scheme() != QLatin1String("http") &&
       baseUrl.scheme() != QLatin1String("https")) ||
      !baseUrl.userInfo().isEmpty() || baseUrl.hasQuery() ||
      baseUrl.hasFragment() ||
      (!basePath.isEmpty() && basePath != QLatin1String("/")))
    return QObject::tr("远端推理服务地址无效（须 http(s)://host[:port]，不带路径参数）");
  if (baseUrl.scheme() == QLatin1String("http") &&
      !isLoopbackHost(baseUrl.host()))
    return QObject::tr("远端推理服务必须使用 https://（http 会明文传输阵列数据与结果）");
  return QString();
}

QString RemotePredictConfig::statusHint() const {
  if (!enabled)
    return QObject::tr("远端预测已禁用，走本地引擎");
  const QString invalid = validate();
  if (!invalid.isEmpty())
    return invalid;
  return QObject::tr("远端预测：%1").arg(baseUrl.toString(QUrl::RemovePath));
}

RemotePredictConfig RemotePredictConfig::fromParts(const QUrl &baseUrl,
                                                   bool enabled,
                                                   const QString &model) {
  RemotePredictConfig config;
  config.baseUrl = baseUrl;
  config.enabled = enabled;
  config.model = model;
  return config;
}

QJsonObject RemotePredictConfig::toJson() const {
  QJsonObject object;
  object.insert(QStringLiteral("base_url"), baseUrl.toString());
  object.insert(QStringLiteral("enabled"), enabled);
  object.insert(QStringLiteral("model"), model);
  return object;
}

RemotePredictConfig RemotePredictConfig::fromJson(const QJsonObject &object) {
  RemotePredictConfig config;
  config.baseUrl =
    QUrl(object.value(QStringLiteral("base_url")).toString().trimmed());
  config.enabled = object.value(QStringLiteral("enabled")).toBool();
  config.model = object.value(QStringLiteral("model")).toString();
  return config;
}

RemotePredictConfig RemotePredictConfig::load() {
  RemotePredictConfig config;
  QFile file(path());
  if (file.open(QIODevice::ReadOnly)) {
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (document.isObject())
      config = fromJson(document.object());
  }
  // 环境变量优先：离线封装/CI 不改用户配置目录也能切换端点（失败均可回溯到
  // env，不在代码里埋第三方地址）。
  const QString url = QString::fromUtf8(qgetenv(kEnvUrl)).trimmed();
  if (!url.isEmpty())
    config.baseUrl = QUrl(url);
  const QString flag = QString::fromUtf8(qgetenv(kEnvEnabled)).trimmed();
  if (flag == QLatin1String("0"))
    config.enabled = false;
  else if (flag == QLatin1String("1"))
    config.enabled = true;
  const QString model = QString::fromUtf8(qgetenv(kEnvModel)).trimmed();
  if (!model.isEmpty())
    config.model = model;
  return config;
}

bool RemotePredictConfig::save(QString *error) const {
  QSaveFile file(path());
  QDir().mkpath(QFileInfo(path()).absolutePath());
  const QByteArray bytes =
    QJsonDocument(toJson()).toJson(QJsonDocument::Compact);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }
  return true;
}
