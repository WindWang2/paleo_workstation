// 层：数据
#include "wellreadiness.h"
#include "catalog/datacatalog.h"
#include "catalog/wellavailability.h"
#include "io/wellfileparsers.h"
#include "io/timedeptool.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <cmath>
#include <string>
#include <vector>

namespace paleo {
namespace {
// 模型侧「已确认可用时深」的井集合，stamp 缓存（areawellprojection 同款：
// data.sqlite(+wal) 的 size/mtime 指纹；确认 / 导入写库即自动失效）。
struct Snapshot {
  QString stamp;
  QSet<qlonglong> usableWells;
  bool opened = false; // 模型存在且本次成功打开（false=无模型/打开失败，走 legacy）
};
QMutex cacheMutex;
QHash<QString, Snapshot> cache;

QString stampOf(const QString &root) {
  QString stamp;
  for (const auto &suffix : {"/data.sqlite", "/data.sqlite-wal"}) {
    const QFileInfo f(root + suffix);
    stamp += QString(":%1:%2:%3:%4").arg(f.size()).arg(f.lastModified().toMSecsSinceEpoch())
        .arg(f.metadataChangeTime().toMSecsSinceEpoch()).arg(f.birthTime().toMSecsSinceEpoch());
  }
  return stamp;
}

// 与 ProjectDataFacade::tdTableFor 的元数据门同口径：单位 / 单双程 / 基准
// 已确认 + 至少两个有限样点（深度换算交给消费端的严格表，这里只判可用性）。
bool relationUsable(const data::TimeDepthTable &t) {
  if ((t.timeUnit != "ms" && t.timeUnit != "s") ||
      (t.timeType != "TWT" && t.timeType != "OWT") || t.timeReference.empty() ||
      (t.depthInfo.depthUnit != "m" && t.depthInfo.depthUnit != "ft"))
    return false;
  int finite = 0;
  for (const auto &p : t.depthTimePairs) {
    if (std::isfinite(p.depthValue) && std::isfinite(p.timeValue) && ++finite >= 2)
      return true;
  }
  return false;
}
} // namespace

QSet<qlonglong> usableTimeDepthWells(data::DataService &service, std::string *error) {
  QSet<qlonglong> out;
  std::string sink;
  std::string &report = error ? *error : sink;
  std::vector<data::DataEntry> entries;
  if (!service.listData("time_depth", 0, entries, report))
    return out;
  for (const auto &entry : entries) {
    if (!entry.wellId || out.contains(entry.wellId))
      continue;
    data::TimeDepthTable table;
    if (service.readTimeDepth(entry.id, table, report) && relationUsable(table))
      out.insert(entry.wellId);
  }
  return out;
}

namespace {

Snapshot modelSnapshot(const QString &projectDir) {
  const QString root = QDir(projectDir).absoluteFilePath(QStringLiteral("artifacts/metadata/area-model"));
  Snapshot out;
  if (!QFileInfo::exists(root + QStringLiteral("/data.sqlite")))
    return out;
  out.stamp = stampOf(root);
  {
    QMutexLocker lock(&cacheMutex);
    const auto found = cache.constFind(root);
    if (found != cache.cend() && found->stamp == out.stamp)
      return *found;
  }
  data::DataService service;
  std::string error;
  if (service.openProjectReadOnly(root.toUtf8().toStdString(), error)) {
    out.opened = true;
    out.usableWells = usableTimeDepthWells(service, &error);
  }
  QMutexLocker lock(&cacheMutex);
  if (cache.size() >= 64)
    cache.clear();
  cache.insert(root, out);
  return out;
}

// Legacy 兜底（无工区模型的井）：主 time_depth 链接的固定列序表可插值即可用
//（口径与 tdTableFor 的 unbound 分支一致；Petrel 导出在 io 层已被整表拒收）。
QString legacyTimeDepthPath(DataCatalog *catalog, const QString &wellId) {
  QString assetId;
  for (const EntityAssetLink &l : catalog->linksForEntity(wellId)) {
    if (l.role == QLatin1String("time_depth") && !l.unresolved && l.isPrimary) {
      assetId = l.assetId;
      break;
    }
  }
  if (assetId.isEmpty())
    return QString();
  const CatalogVersion v = catalog->currentVersion(assetId);
  if (v.id.isEmpty())
    return QString();
  return DataCatalog::resolvedVersionPath(catalog->projectDir(), v);
}
} // namespace

QHash<QString, WellReadiness> analysisReadiness(DataCatalog *catalog,
                                                const QString &projectDir) {
  QHash<QString, WellReadiness> out;
  if (!catalog)
    return out;
  const Snapshot snapshot = modelSnapshot(projectDir);
  for (const CatalogEntity &well : catalog->entities(QStringLiteral("well"))) {
    WellReadiness r;
    r.hasCurves = wellSupportsAnalysis(catalog, well, /*requireCurves=*/true);
    const qlonglong modelId = well.extra.value(QStringLiteral("areaModelWellId")).toLongLong();
    if (modelId > 0) {
      r.tdUsable = snapshot.usableWells.contains(modelId);
    } else {
      const QString path = legacyTimeDepthPath(catalog, well.id);
      if (!path.isEmpty()) {
        TimeDepthTable table;
        QString error;
        if (readTimeDepth(path, table, &error)) {
          for (const auto &row : table.rows) {
            if (row.hasMd && std::isfinite(row.md) && std::isfinite(row.timeMs)) {
              r.tdUsable = TimeDepthTool::interpolateTimeMs(table, row.md, true).ok();
              break;
            }
          }
        }
      }
    }
    out.insert(well.id, r);
  }
  return out;
}

} // namespace paleo
