// 层：数据
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace paleo::chat {
inline constexpr int kResultHardBytes = 64 * 1024;
inline constexpr int kResultPageBytes = 8 * 1024;
inline constexpr int kMinimumEnvelopeBytes = 768;

QByteArray compactJson(const QJsonValue &value);
QString jsonValueKind(const QJsonValue &value);
QJsonObject resultEnvelope(const QJsonValue &value, bool ok);
QJsonObject transportResult(const QJsonObject &facts, const QString &code,
                            const QJsonValue &summary = QJsonValue());
QJsonObject resultError(const QString &code);
QJsonObject toolResultEnvelopeSchema(const QJsonObject &nativeSchema = {});
bool isResultEnvelope(const QJsonObject &value);
} // namespace paleo::chat
