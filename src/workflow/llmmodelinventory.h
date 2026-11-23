// 层：功能
#pragma once
#include "llmdiagnosticcontract.h"
#include <QObject>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace paleo::chat {

// H6-W3：有界模型目录发现（显式启动；GET <endpoint>/models）。
//
// 边界（与 LlmClient 同纪律，独立实现——LlmClient 只读不复改）：
//   · 端点拼接遵循生产 base path 语义（尾斜杠容错，同 completionsUrl）；
//     不接受模型给出的发现 URL，不扫描常见 provider 地址。
//   · https / loopback http / 显式 allowInsecureHttp 三界沿用生产规则
//     （本地结构校验由调用方借 LlmConfig::validate 完成；本类只发合法候选）。
//   · 手动重定向策略：不跟随跨源/同源 Location，不重发凭据；3xx 如实报错。
//   · 双时限（空闲/总）+ 累计字节预算（readyRead 与 tail 同一把尺子）。
//   · 仅解析已声明的有限目录形状：对象含 "data" 数组，元素为含非空 "id"
//     字符串的对象。未知形状/空目录/404/405/无权限各自区分（枚举见契约）。
//   · 有界：条目数、model ID 字符数、响应字节都钳制；截断如实标注 partial。
//   · 凭据防回显：条目/对象键回显本次凭据即剔除（credentialFiltered），
//     错误文本出报告前统一脱敏（llmcredentialguard）。
//   · 单次终态：在途重复启动被忽略；cancel/析构安全终止在途请求。
//   · 目录只提供候选：目录 unsupported/unknown 不代表文本能力不可用，
//     也不自动触发任何后续请求。
class LlmModelInventory : public QObject {
  Q_OBJECT
public:
  struct Limits {
    int idleTimeoutMs = 10000;
    int totalTimeoutMs = 20000;
    qint64 maxBytes = 2ll * 1024 * 1024;
    int maxEntries = 1000;
    int maxIdChars = 512;
  };

  // 测试注入口：注入自定义 QNetworkAccessManager（生产用默认构造）。
  explicit LlmModelInventory(Limits limits,
                             QNetworkAccessManager *namForTesting = nullptr,
                             QObject *parent = nullptr);
  ~LlmModelInventory() override;

  // 显式发起一次目录 GET。在途时再次调用被忽略（单次终态纪律）。
  // 候选端点必须已过本地结构校验；key 可空 = 不带 Authorization 头。
  void probe(const LlmDiagnosticCandidate &candidate);
  void cancel();
  bool busy() const { return m_reply != nullptr; }

  // <endpoint>/models 的生产拼接（尾斜杠容错；仅供测试对账）。
  static QUrl modelsUrl(const QUrl &endpoint);

signals:
  void finished(const paleo::chat::LlmCatalogObservation &observation);

private:
  void finishWith(LlmCatalogObservation observation);
  bool countBytes(qint64 bytes);
  LlmCatalogObservation parseDocument(const QByteArray &body,
                                      const QByteArray &apiKey) const;

  Limits m_limits;
  QNetworkAccessManager *m_nam = nullptr;
  bool m_ownedNam = false;
  QPointer<QNetworkReply> m_reply = nullptr;
  QTimer *m_idleTimer = nullptr;
  QTimer *m_totalTimer = nullptr;
  QByteArray m_body;
  qint64 m_received = 0;
  bool m_finished = false; // 单次终态守卫（先到者胜）
  QByteArray m_key; // 本次请求凭据（脱敏基准；不落盘）
};

} // namespace paleo::chat
