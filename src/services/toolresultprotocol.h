// 层：数据
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace paleo::chat {
QJsonObject boundedWellResultMetadata(const QJsonObject &native);
inline constexpr int kResultHardBytes = 64 * 1024;
inline constexpr int kResultPageBytes = 8 * 1024;
inline constexpr int kMinimumEnvelopeBytes = 768;

QByteArray compactJson(const QJsonValue &value);
QString jsonValueKind(const QJsonValue &value);
QJsonObject resultEnvelope(const QJsonValue &value, bool ok);
// H7：最终可发送 envelope 的精确值——resultEnvelope(data, ok) 加上
// transport.maxBytes，即 ChatResultStore::encode 首段的成功形态。页生成在
// 选页前用它给「最终投递物」计价，不再用固定预留猜外层成本。
QJsonObject resultEnvelopeForBudget(const QJsonValue &value, bool ok, int maxBytes);
int resultEnvelopeBytes(const QJsonValue &value, bool ok, int maxBytes);
// 宿主侧投递失败（配额漂移等）已是终态 envelope，调用方不得再包一层。
// retryAdvice 按失败性质给出：读页预算不足时重试同一 cursor 是只读操作，
// 不能沿用默认的 do_not_repeat_execution。
QJsonObject boundedResultError(const QString &code, int maxBytes,
                               const QString &retryAdvice =
                                   QStringLiteral("do_not_repeat_execution"));
QJsonObject transportResult(const QJsonObject &facts, const QString &code,
                            const QJsonValue &summary = QJsonValue());
QJsonObject resultError(const QString &code);
QJsonObject toolResultEnvelopeSchema(const QJsonObject &nativeSchema = {});
bool isResultEnvelope(const QJsonObject &value);
} // namespace paleo::chat
