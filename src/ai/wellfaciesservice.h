// 层：功能
#pragma once
#include "domain/welllogfacies.h"
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>
class QNetworkReply;

// 测井相预测服务配置（#133）。
//  - 无缺省地址：首次使用必须在「预测服务」里显式填写，代码/文档不再硬编码
//    任何第三方地址。
//  - 传输安全：只接受 https://；http:// 仅在 loopback（localhost/127.x/::1）
//    或显式开启 allowInsecureHttp 时放行——开启意味着 API 密钥与整井曲线
//    明文过网，UI 与状态栏会提示。
//  - 密钥存储：构建带 QtKeychain（PALEO_HAVE_QTKEYCHAIN）时进系统钥匙串
//    （Windows 凭据管理器 / libsecret / macOS 钥匙串），JSON 只存地址与开关；
//    无 QtKeychain 时回落 JSON（POSIX 0600），见 docs/WELL_FACIES_PREDICTION.md。
struct WellFaciesConfig {
  QUrl baseUrl;
  QByteArray apiKey;
  bool allowInsecureHttp = false;
  static QString path();
  // 读 JSON + 环境变量（PALEO_WELL_FACIES_URL / _API_KEY /
  // _ALLOW_INSECURE_HTTP=1）。钥匙串里的密钥由 WellFaciesKeyStore 异步补齐。
  static WellFaciesConfig load();
  // 落盘地址与开关；writeKeyToFile=false 时 JSON 不含密钥（钥匙串路线）。
  bool save(QString *error, bool writeKeyToFile = true) const;
  // 地址合法性与传输安全校验；空串 = 通过。
  static QString validateUrl(const QUrl &url, bool allowInsecureHttp);
  static bool isLoopbackHost(const QString &host);
  // 响应体上限（字节）：超出即判失败，防恶意/故障服务耗尽内存。
  static constexpr qint64 kMaxResponseBytes = 64ll * 1024 * 1024;
};
class WellFaciesService : public QObject {
  Q_OBJECT
public:
  explicit WellFaciesService(QObject *parent = nullptr);
  ~WellFaciesService() override { cancel(); }
  void configure(const WellFaciesConfig &config);
  void fetchModels();
  void predict(const WellFaciesModel &model, const QString &wellName,
               const WellFaciesInput &input);
  void cancel();
signals:
  void modelsReady(const QVector<WellFaciesModel> &models);
  void completed(const WellFaciesResult &result);
  void failed(const QString &reason);
  void progress(int percent, const QString &message);

private:
  void request(const QString &path, const QJsonObject &body, bool post,
               bool models);
  void handlePrediction(const QJsonObject &json);
  void fail(const QString &reason);
  QNetworkAccessManager m_network;
  QPointer<QNetworkReply> m_reply;
  QTimer m_poll;
  QElapsedTimer m_elapsed;
  WellFaciesConfig m_config;
  WellFaciesInput m_input;
  QString m_wellName, m_jobId, m_modelId;
  int m_generation = 0;
};
