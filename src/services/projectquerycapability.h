// 层：数据
#pragma once
#include <QJsonObject>
#include <QString>
#include <memory>
class DataCatalog;

namespace paleo::query {
struct Scope {
  QString documentId;
  quint64 epoch = 0, contextGeneration = 0;
  QString chatSessionId{};
  bool operator==(const Scope &) const = default;
};
struct QueryPolicy {
  int maxCursors = 128;
  qint64 ttlMs = 30 * 60 * 1000;
  qint64 maxCacheBytes = 8 * 1024 * 1024;
  int maxTraversalNodes = 4096;
};
// Read-only metadata capability. Continuations are opaque and bounded. The most
// recently consumed page can be replayed until the following page is consumed.
// The native version.describe.v1 object is intentionally not modified here.
class ProjectQueryCapability {
public:
  explicit ProjectQueryCapability(QueryPolicy policy = {});
  ~ProjectQueryCapability();
  void bind(DataCatalog *catalog, Scope scope);
  void clear();
  QJsonObject query(const QJsonObject &arguments);
  QJsonObject lineage(const QJsonObject &arguments);
private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
}
