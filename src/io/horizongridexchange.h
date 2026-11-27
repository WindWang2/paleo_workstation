// 层：数据
#pragma once
#include <QVariantMap>
#include <functional>

namespace paleo::io {
struct InterpretationPoint {
    int inlineNumber, crosslineNumber;
    double twtMs, confidence;
};
bool isHorizonGridExchange(const QString &path);
bool writeHorizonGridExchange(const QString &verifiedCsv, const QString &destination,
    const QVariantMap &source, QString *error, const std::function<bool()> &cancelled = {});
// Callback rows are provisional until both final integrity checks succeed.
bool readHorizonGridExchange(const QString &path, const QString &expectedFileSha, QVariantMap &source,
    QString *error, const std::function<bool()> &cancelled = {},
    const std::function<void(const InterpretationPoint &)> &point = {});
}
