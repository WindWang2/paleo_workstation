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
// #283 shapefile 族附属后缀：成员与主 .shp 同目录同主名、无独立 catalog
// 记录，但 OGR 打开图层缺一不可——必须随主件同生共死。覆盖 ingestplan 归组
// 口径（shp/shx/dbf/prj）外加 .cpg 与常见索引/元数据后缀。
const QStringList shpSidecarSuffixes = {
  QStringLiteral("shx"), QStringLiteral("dbf"), QStringLiteral("prj"),
  QStringLiteral("cpg"), QStringLiteral("sbn"), QStringLiteral("sbx"),
  QStringLiteral("fbn"), QStringLiteral("fbx"), QStringLiteral("ain"),
  QStringLiteral("aih"), QStringLiteral("atx"), QStringLiteral("qix"),
  QStringLiteral("ixs"), QStringLiteral("mxs")};
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
  const QString projectCanonical = PathCanon::canonicalize(s.projectDir);
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
    // #283：shapefile 族附属文件（.shx/.dbf/.prj/.cpg…）没有 catalog 记录，
    // 但与主 .shp 同生共死——主件在工程内时把同目录同主名的成员一并登记为
    // 被引用（不进孤儿清单），并记入 versionSidecars 供 purge 连带回收。
    // 成员若已登记为某个版本文件，登记为被引用是同值幂等，purge 侧会在
    // preview 里跳过（它是别人的版本字节，不能连带删）。
    if (f.sizeBytes >= 0 && !f.canonicalPath.isEmpty()
        && f.canonicalPath.startsWith(projectCanonical + QLatin1Char('/'))
        && QFileInfo(path).suffix().compare(QLatin1String("shp"), Qt::CaseInsensitive) == 0) {
      const QDir vdir = QFileInfo(path).absoluteDir();
      const QString base = QFileInfo(path).completeBaseName();
      // ingestplan 归组大小写不敏感（.SHP/.SHX 同主名即成族），族成员按原名
      // 落位受管版本目录——成员探测同样大小写不敏感，否则大写族附属（如
      // BOUNDARY.SHX）仍是孤儿、仍会被误删（Linux 大小写敏感文件系统现形）。
      const QString xmlName = base + QStringLiteral(".shp.xml");
      for (const auto &entry : vdir.entryList(QDir::Files)) {
        if (entry.compare(xmlName, Qt::CaseInsensitive) != 0) {
          const QFileInfo ei(entry);
          if (ei.completeBaseName().compare(base, Qt::CaseInsensitive) != 0) continue;
          if (!shpSidecarSuffixes.contains(ei.suffix().toLower())) continue;
        }
        const QFileInfo si(vdir.filePath(entry));
        if (!si.isFile() || si.isSymLink()) continue;
        const QString c = PathCanon::canonicalize(si.absoluteFilePath());
        if (c.isEmpty()) continue;
        references.insert(c);
        r.versionSidecars[v.id].append(fact(si.absoluteFilePath(), s.projectDir));
      }
    }
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
  // 全部版本文件的身份集：附属成员若自身已登记为版本文件，是别人的版本
  // 字节，不能随主件连带删（purgeManagedFiles 对 retained 版本引用还有
  // 一道同值保护，这里保证它不进待删清单、不误导字节统计）。
  const QSet<QString> registered(r.canonicalReferences.begin(), r.canonicalReferences.end());
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
    // #283：shapefile 族附属文件随主件同生共死——主件入列回收时成员一并入列。
    for (const auto &m : r.versionSidecars.value(v.version.id)) {
      if (registered.contains(m.canonicalPath)) continue;
      if (!selectedFiles.contains(m.canonicalPath)) {
        selectedFiles.insert(m.canonicalPath); p.files.append(m); p.bytes += m.sizeBytes;
      }
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
