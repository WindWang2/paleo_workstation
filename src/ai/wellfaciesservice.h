// 层：功能
#pragma once
#include "domain/welllogfacies.h"
#include <QElapsedTimer>
#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QStringList>
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
  // 缺省 <GenericConfigLocation>/paleo/well-facies.json；
  // PALEO_WELL_FACIES_CONFIG 非空时用其全路径。
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
  // #479：已受理缓存的登记/注销（带上限 FIFO 淘汰，见 kMaxAcceptedJobs）。
  void rememberAcceptedJob(const QString &key, const QString &jobId);
  void forgetAcceptedJob(const QString &key);
  QNetworkAccessManager m_network;
  QPointer<QNetworkReply> m_reply;
  QTimer m_poll;
  QElapsedTimer m_elapsed;
  WellFaciesConfig m_config;
  WellFaciesInput m_input;
  QString m_wellName, m_jobId, m_modelId, m_key;
  // 已受理未终态的任务，键 = 模型+井+输入的 SHA-256：逐地层组预测时停掉再点
  // 「预测相」，能续查任一在途地层的任务而不重复提交。
  // #479：任务终态才摘键——中途退出/页面切走会留下永不终结的键，只增不减；
  // 超过 kMaxAcceptedJobs 按插入序淘汰最旧（续查面有限，FIFO 足够）。
  static constexpr int kMaxAcceptedJobs = 32;
  QHash<QString, QString> m_accepted;
  QStringList m_acceptOrder; // m_accepted 的插入序（FIFO 淘汰用）
  int m_generation = 0;
};
