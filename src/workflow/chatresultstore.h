// 层：功能
#pragma once
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <functional>

namespace paleo::chat {
// Session-owned immutable report values. No project writes and no path-backed handles.
class ChatResultStore {
public:
  struct Limits {
    int resultBytes = 1024 * 1024;
    int totalBytes = 8 * 1024 * 1024;
    int handles = 128;
    qint64 ttlMs = 30 * 60 * 1000;
  };
  using Clock = std::function<qint64()>;
  ChatResultStore();
  explicit ChatResultStore(Limits limits, Clock clock = {});
  void setScope(const QString &session, const QString &document, quint64 epoch);
  QJsonObject encode(const QString &rawJson, bool ok, int maxBytes, bool allowCache = true);
  // hostQuotaBytes <= 0：旧调用方，只按请求 max_bytes 计价。
  // hostQuotaBytes > 0：宿主本次调用实际可发送的最终 paleo.tool-result.v1
  // envelope 字节上限（controller 可信状态给出，不读 args/历史）。页生成按
  // min(request.max_bytes, hostQuotaBytes) 与协议硬上限的交集精确计价。
  QJsonObject read(const QJsonObject &request, int hostQuotaBytes = 0);
  // 上一次 read 实际采用的 envelope 预算（请求上限与宿主配额的交集）；
  // 0 = 未进入分页（引用/参数校验失败）。最终化必须按这个值复核，
  // 否则 request.max_bytes 小于宿主配额时会用更大的值重包。
  int lastReadEnvelopeBytes() const { return m_lastReadEnvelopeBytes; }
  int retainedBytes() const { return m_bytes; }
  int retainedResults() const { return int(m_values.size()); }

private:
  struct Stored { QJsonValue value; int bytes; qint64 expires; };
  struct Position { int offset = 0; QString mode; };
  QString put(const QJsonValue &value, const QByteArray &json, QString *error);
  QJsonObject readValue(const QJsonObject &request, const QString &reference,
                       const QJsonValue &value, int envelopeBytes);
  QJsonObject containerPage(const QString &reference, const QString &selector,
                            const QJsonValue &value, Position position,
                            int pageSize, int envelopeBytes);
  QJsonObject stringPage(const QString &reference, const QString &selector,
                         const QString &value, Position position, int envelopeBytes);
  QString cursor(const QString &reference, const QString &selector,
                 const QString &mode, int offset) const;
  bool decodeCursor(const QString &token, const QString &reference,
                    const QString &selector, Position *position) const;
  static bool select(const QJsonValue &root, const QString &selector, QJsonValue *out);
  void expire();
  Limits m_limits;
  Clock m_clock;
  QString m_session, m_document, m_scope;
  quint64 m_epoch = 0;
  QByteArray m_secret;
  QHash<QString, Stored> m_values;
  QHash<QString, bool> m_expired;
  int m_bytes = 0;
  int m_lastReadEnvelopeBytes = 0;
};
} // namespace paleo::chat
