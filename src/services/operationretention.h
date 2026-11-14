// 层：数据
#pragma once
#include <QByteArray>
#include <QSet>
#include <QStringList>
#include <functional>
class QJsonObject;

namespace paleo::storage {
QString operationPlanHash(const QJsonObject &frozen, const QJsonObject &stage,
    const QJsonObject &presentation, const QJsonObject &external);
struct OperationRetention {
  bool complete = false;
  QString code;
  QByteArray revision;
  QStringList paths, versionIds;
  QSet<QString> canonicalRoots;
  // Targets own their version/private directory, including compound sidecars.
  bool protectsPath(const QString &absolutePath) const;
};
// A missing consumed/expected-head version makes its physical roots unknowable.
void verifyOperationVersions(OperationRetention &retention,const QSet<QString> &knownVersions);
// Control metadata only. Every schema-v2 record retains its references,
// regardless of execution/commit/presentation state. Unknown facts block GC.
OperationRetention operationRetention(const QString &projectRoot,
    const std::function<bool()> &cancelled = {});
}
