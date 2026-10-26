// 层：数据
#pragma once
#include "../domain/projectclassifier.h"
#include <functional>
#include <QVariantMap>
class QIODevice;

namespace paleo::io {
inline constexpr qint64 kScanPrefixBytes = 256 * 1024;
struct SourceProbe {
  ProjectClassification classification;
  QString wellName, error;
  QString prefixSha256;
  qint64 bytesRead = 0;
  bool limited = false;
  bool cancelled = false;
};
// Identification candidates only. Never validates samples or hashes a full file.
// The device overload permits read-budget tests without timing assumptions.
SourceProbe probeSource(QIODevice &source, const QString &path,
                        const std::function<bool()> &cancelled = {});
SourceProbe probeSource(const QString &path, const std::function<bool()> &cancelled = {});
QVariantMap sourceSnapshot(const QString &path, const SourceProbe &probe);
bool matchesSourceSnapshot(const QString &path, const QVariantMap &snapshot);
}
