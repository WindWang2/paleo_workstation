// 层：数据
#include "storagegovernance.h"
#include "../io/pathcanon.h"
#include <QDirIterator>
#include <QCoreApplication>
#include <QSet>

namespace paleo::storage {
namespace {
const QStringList roots = {QStringLiteral("raw"), QStringLiteral("derived"),
  QStringLiteral("intermediate"), QStringLiteral("output"),
  QStringLiteral("artifacts/RAW"), QStringLiteral("artifacts/DERIVED"),
  QStringLiteral("artifacts/INTERMEDIATE"), QStringLiteral("artifacts/OUTPUT"),
  QStringLiteral("artifacts/raw"), QStringLiteral("artifacts/derived"),
  QStringLiteral("artifacts/intermediate"), QStringLiteral("artifacts/output")};
FileFact fact(const QString &path, const QString &projectDir) {
  FileFact f;
  f.relativePath = QDir(projectDir).relativeFilePath(path);
  f.canonicalPath = PathCanon::canonicalize(path);
  const QFileInfo fi(path);
  if (fi.isFile()) { f.sizeBytes = fi.size(); f.modified = fi.lastModified(); }
  return f;
}
bool inRoots(const QString &relative) {
  for (const auto &root : roots)
    if (relative.startsWith(root + QLatin1Char('/'))) return true;
  return false;
}
bool safeFile(const FileFact &f, const QString &projectDir) {
  CatalogVersion v;
  v.path = f.relativePath;
  const QString path = DataCatalog::resolvedVersionPath(projectDir, v);
  if (path.isEmpty() || !inRoots(f.relativePath)) return false;
  const FileFact now = fact(path, projectDir);
  return now.canonicalPath == f.canonicalPath && now.sizeBytes == f.sizeBytes
    && now.modified == f.modified && now.sizeBytes >= 0;
}
}
Snapshot snapshot(DataCatalog *cat) {
  Snapshot s;
  if (!cat || !cat->isOpen()) return s;
  s.catalogPath = cat->catalogPath();
  QDir dir(QFileInfo(s.catalogPath).absolutePath());
  dir.cdUp(); dir.cdUp();
  s.projectDir = dir.absolutePath();
  s.mutationSeq = cat->mutationSeq();
  s.assets = cat->assets(); s.entities = cat->entities();
  s.versions = cat->versions(); s.links = cat->links();
  return s;
}
Report scan(const Snapshot &s, const Progress &progress) {
  Report r; r.source = s;
  if (s.catalogPath.isEmpty()) { r.uncovered << QCoreApplication::translate("StorageGovernance", "工程未打开"); return r; }
  QHash<QString, CatalogAsset> assets;
  QHash<QString, QSet<QString>> entities;
  for (const auto &a : s.assets) assets.insert(a.id, a);
  for (const auto &l : s.links)
    if (!l.unresolved && !l.entityId.isEmpty()) entities[l.assetId].insert(l.entityId);
  QSet<QString> references;
  int done = 0;
  for (const auto &v : s.versions) {
    if (progress && !progress(done, s.versions.size(), v.fileName)) return r;
    if (v.managed && !inRoots(v.path))
      r.uncovered << QCoreApplication::translate("StorageGovernance", "受管版本目录未覆盖：%1").arg(v.path);
    const QString path = DataCatalog::resolvedVersionPath(s.projectDir, v);
    // Even unsafe/symlink aliases remain references: never report their target
    // as unreferenced. Safety validation controls whether cleanup is allowed.
    const QString canonical = PathCanon::canonicalize(v.path, v.managed ? s.projectDir : QString());
    r.canonicalReferences.insert(v.id, canonical);
    if (!canonical.isEmpty()) references.insert(canonical);
    FileFact f;
    if (!path.isEmpty()) f = fact(path, s.projectDir);
    r.versionFiles.insert(v.id, f);
    if (f.sizeBytes >= 0) {
      r.versionBytes += f.sizeBytes;
      r.bytesByType[assets.value(v.assetId).type] += f.sizeBytes;
      const auto linkedEntities = entities.value(v.assetId);
      if (linkedEntities.isEmpty()) r.bytesByEntity[QString()] += f.sizeBytes;
      for (const auto &id : linkedEntities) r.bytesByEntity[id] += f.sizeBytes;
    } else ++r.unknownSizes;
    if (v.stage == QLatin1String("DERIVED") && v.extra.value(QStringLiteral("stale")).toBool())
      r.stale.append({v, assets.value(v.assetId).displayName,
        v.extra.value(QStringLiteral("staleReason")).toString(), f});
    ++done;
  }
  QSet<QString> visited;
  for (const auto &root : roots) {
    const QString path = QDir(s.projectDir).filePath(root);
    const QFileInfo info(path);
    if (!info.exists()) continue;
    CatalogVersion probe; probe.path = root;
    if (info.isSymLink() || DataCatalog::resolvedVersionPath(s.projectDir, probe).isEmpty()
        || !info.isDir() || !info.isReadable()) {
      r.uncovered << root; continue;
    }
    const QString canonicalRoot = PathCanon::canonicalize(path);
    if (visited.contains(canonicalRoot)) continue;
    visited.insert(canonicalRoot); r.scannedRoots << root;
    // No FollowSymlinks. Enumerate directories too so excluded coverage is honest.
    QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
      const QString candidate = it.next();
      if (progress && !progress(++done, 0, QDir(s.projectDir).relativeFilePath(candidate))) return r;
      const QFileInfo fi = it.fileInfo();
      if (fi.isSymLink() || (fi.isDir() && !fi.isReadable())) {
        r.uncovered << QDir(s.projectDir).relativeFilePath(candidate); continue;
      }
      if (!fi.isFile()) continue;
      const FileFact f = fact(candidate, s.projectDir);
      if (!references.contains(f.canonicalPath)) r.orphans.append(f);
    }
  }
  r.complete = r.uncovered.isEmpty();
  return r;
}
Preview preview(const Report &r, const QStringList &versionIds, const QStringList &orphanPaths) {
  Preview p; p.source = r.source;
  if (!r.complete) { p.blocked << QCoreApplication::translate("StorageGovernance", "扫描未完成或存在未覆盖目录，请重新扫描"); return p; }
  QSet<QString> selected(versionIds.begin(), versionIds.end());
  QSet<QString> available;
  for (const auto &v : r.stale) available.insert(v.version.id);
  for (const auto &id : selected)
    if (!available.contains(id)) p.blocked << QCoreApplication::translate("StorageGovernance", "版本未列为 stale：%1").arg(id);
  for (const auto &v : r.source.versions)
    for (const auto &parent : v.parentVersionIds)
      if (selected.contains(parent) && !selected.contains(v.id))
        p.blocked << QCoreApplication::translate("StorageGovernance", "版本 %1 仍被 %2 引用").arg(parent, v.id);
  QSet<QString> remainingReferences, selectedFiles;
  for (const auto &v : r.source.versions) {
    const QString canonical = r.canonicalReferences.value(v.id);
    if (!selected.contains(v.id) && !canonical.isEmpty()) remainingReferences.insert(canonical);
  }
  for (const auto &v : r.stale) {
    if (!selected.contains(v.version.id)) continue;
    p.versionIds << v.version.id;
    ++p.affectedVersions;
    // External sources and files shared with retained versions are never deleted.
    if (!v.version.managed || remainingReferences.contains(v.file.canonicalPath)) continue;
    if (v.file.sizeBytes < 0) { p.blocked << QCoreApplication::translate("StorageGovernance", "文件大小未知：%1").arg(v.version.id); continue; }
    if (!selectedFiles.contains(v.file.canonicalPath)) {
      selectedFiles.insert(v.file.canonicalPath); p.files.append(v.file); p.bytes += v.file.sizeBytes;
    }
  }
  QSet<QString> wanted(orphanPaths.begin(), orphanPaths.end());
  for (const auto &f : r.orphans) if (wanted.remove(f.relativePath)) {
    p.orphanFiles.append(f);
    if (!selectedFiles.contains(f.canonicalPath)) {
      selectedFiles.insert(f.canonicalPath); p.files.append(f); p.bytes += f.sizeBytes;
    }
  }
  if (!wanted.isEmpty()) p.blocked << QCoreApplication::translate("StorageGovernance", "所选文件已不在未引用清单中");
  p.valid = p.blocked.isEmpty() && (!p.versionIds.isEmpty() || !p.orphanFiles.isEmpty());
  return p;
}
bool validate(const Preview &p, const Progress &progress, QString *error) {
  if (!p.valid) { if (error) *error = QCoreApplication::translate("StorageGovernance", "预览无效"); return false; }
  int done = 0;
  for (const auto &f : p.files) {
    if (progress && !progress(done++, p.files.size(), f.relativePath)) {
      if (error) *error = QCoreApplication::translate("StorageGovernance", "已取消，未执行任何回收动作"); return false;
    }
    if (!safeFile(f, p.source.projectDir)) {
      if (error) *error = QCoreApplication::translate("StorageGovernance", "预览后文件变化或路径不安全：%1").arg(f.relativePath); return false;
    }
  }
  return true;
}
} // namespace paleo::storage
