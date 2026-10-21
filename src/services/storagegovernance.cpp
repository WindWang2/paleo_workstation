// 层：数据
#include "storagegovernance.h"
#include "../domain/sidecarvocab.h"
#include "../io/pathcanon.h"
#include <QDirIterator>
#include <QCoreApplication>
#include <QSet>

namespace paleo::storage {
namespace {
QString canonicalIdentity(const QString &path, const QString &base = {}) {
  auto result = PathCanon::canonicalize(path, base);
#ifdef Q_OS_WIN
  // RAW/raw roots resolve to the same NTFS directory. QFileInfo can retain
  // the caller's spelling, so reference and scan keys must ignore case.
  result = result.toCaseFolded();
#endif
  return result;
}
const QStringList roots = {QStringLiteral("raw"), QStringLiteral("derived"),
  QStringLiteral("intermediate"), QStringLiteral("output"),
  QStringLiteral("artifacts/RAW"), QStringLiteral("artifacts/DERIVED"),
  QStringLiteral("artifacts/INTERMEDIATE"), QStringLiteral("artifacts/OUTPUT"),
  QStringLiteral("artifacts/raw"), QStringLiteral("artifacts/derived"),
  QStringLiteral("artifacts/intermediate"), QStringLiteral("artifacts/output")};
FileFact fact(const QString &path, const QString &projectDir) {
  FileFact f;
  f.relativePath = QDir(projectDir).relativeFilePath(path);
  f.canonicalPath = canonicalIdentity(path);
  const QFileInfo fi(path);
  if (fi.isFile()) { f.sizeBytes = fi.size(); f.modified = fi.lastModified(); }
  return f;
}
bool inRoots(const QString &relative) {
  for (const auto &root : roots)
    if (relative.startsWith(root + QLatin1Char('/'))) return true;
  return false;
}
// #283 + B-03/A-14：附属文件词表统一在 domain/sidecarvocab.h——shapefile
// 族（OGR 必备件 + 索引/样式/元数据）与「主件全名+后缀串」特判（.xml/
// .aux.xml）单一事实源，与 paleomapfiles 导入侧并集，防两侧再分叉。
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
  const QString projectCanonical = canonicalIdentity(s.projectDir);
  int done = 0;
  for (const auto &v : s.versions) {
    if (progress && !progress(done, s.versions.size(), v.fileName)) return r;
    if (v.managed && !inRoots(v.path))
      r.uncovered << QCoreApplication::translate("StorageGovernance", "受管版本目录未覆盖：%1").arg(v.path);
    const QString path = DataCatalog::resolvedVersionPath(s.projectDir, v);
    // Even unsafe/symlink aliases remain references: never report their target
    // as unreferenced. Safety validation controls whether cleanup is allowed.
    const QString canonical = canonicalIdentity(v.path, v.managed ? s.projectDir : QString());
    r.canonicalReferences.insert(v.id, canonical);
    if (!canonical.isEmpty()) references.insert(canonical);
    FileFact f;
    if (!path.isEmpty()) f = fact(path, s.projectDir);
    r.versionFiles.insert(v.id, f);
    // Dataset manifests keep coordinate/style members alive with their main file.
    QSet<QString> registeredCanonical; // 本版本已登记引用的 canonical（防族扫描重复入账）
    for (const auto &entry : v.extra.value("bundle_members").toList()) {
      const auto name = entry.toMap().value("file").toString();
      if (!v.managed || !DataCatalog::isSafePathSegment(name) || name == QFileInfo(path).fileName()) continue;
      const auto member = QFileInfo(path).absoluteDir().filePath(name); const auto sidecar = fact(member, s.projectDir);
      if (sidecar.sizeBytes < 0 || !sidecar.canonicalPath.startsWith(projectCanonical + '/')) continue;
      references.insert(sidecar.canonicalPath); registeredCanonical.insert(sidecar.canonicalPath);
      r.versionSidecars[v.id].append(sidecar);
    }
    // #283 + B-03/A-14：附属文件族扫描——bundle 与非 bundle 版本同权（此前
    // 以 bundle_members.isEmpty() 为门，bundle 导入后由 QGIS/GDAL 生成的
    // .aux.xml/.qix/.qml/.tfw 全部误报孤儿，用户确认回收即删）。主件后缀
    // 分派 shp 族/栅格族词表；成员若已由 bundle_members 登记，跳过（幂等）。
    // 成员若已登记为某个版本文件，登记为被引用是同值幂等，purge 侧会在
    // preview 里跳过（它是别人的版本字节，不能连带删）。
    if (f.sizeBytes >= 0 && !f.canonicalPath.isEmpty()
        && f.canonicalPath.startsWith(projectCanonical + QLatin1Char('/'))) {
      const QString mainSuffix = QFileInfo(path).suffix().toLower();
      const bool isShp = mainSuffix == QLatin1String("shp");
      const bool isRaster = mainSuffix == QLatin1String("tif") || mainSuffix == QLatin1String("tiff")
        || mainSuffix == QLatin1String("img") || mainSuffix == QLatin1String("png")
        || mainSuffix == QLatin1String("jpg") || mainSuffix == QLatin1String("jpeg");
      if (isShp || isRaster) {
        const QDir vdir = QFileInfo(path).absoluteDir();
        const QString base = QFileInfo(path).completeBaseName();
        const QString mainName = QFileInfo(path).fileName();
        const QStringList &sidecarSuffixes = isShp ? paleo::sidecars::shpFamily() : paleo::sidecars::rasterFamily();
        // ingestplan 归组大小写不敏感（.SHP/.SHX 同主名即成族），族成员按原名
        // 落位受管版本目录——成员探测同样大小写不敏感，否则大写族附属（如
        // BOUNDARY.SHX）仍是孤儿、仍会被误删（Linux 大小写敏感文件系统现形）。
        for (const auto &entry : vdir.entryList(QDir::Files)) {
          const QFileInfo ei(entry);
          bool hit = false;
          if (ei.completeBaseName().compare(base, Qt::CaseInsensitive) == 0
              && sidecarSuffixes.contains(ei.suffix().toLower()))
            hit = true;
          else
            for (const auto &nameSuffix : paleo::sidecars::nameSuffixes())
              if (entry.compare(mainName + QLatin1Char('.') + nameSuffix, Qt::CaseInsensitive) == 0) { hit = true; break; }
          if (!hit) continue;
          const QFileInfo si(vdir.filePath(entry));
          if (!si.isFile() || si.isSymLink()) continue;
          const QString c = canonicalIdentity(si.absoluteFilePath());
          if (c.isEmpty() || registeredCanonical.contains(c)) continue;
          references.insert(c);
          r.versionSidecars[v.id].append(fact(si.absoluteFilePath(), s.projectDir));
        }
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
    const QString canonicalRoot = canonicalIdentity(path);
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
