// 层：功能
#include "assetops.h"
#include "../io/pathcanon.h"
#include "../services/paleomapfiles.h"

#include "../catalog/datacatalog.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSet>

namespace paleo::assetops
{

namespace
{

// A compound dataset is one asset; only safe, registered members in its own
// managed directory may be purged. External originals are never expanded.
QStringList versionFiles(const QString &project, const CatalogVersion &version)
{
  const auto primary = DataCatalog::resolvedVersionPath(project, version);
  if (primary.isEmpty()) return {};
  QStringList paths{primary};
  if (!version.managed) return paths;
  for (const auto &value : version.extra.value("bundle_members").toList()) {
    const auto name = value.toMap().value("file").toString();
    if (!DataCatalog::isSafePathSegment(name)) continue;
    auto member = version;
    member.path = QFileInfo(version.path).dir().filePath(name);
    const auto path = DataCatalog::resolvedVersionPath(project, member);
    if (!path.isEmpty()) paths << path;
  }
  paths.removeDuplicates(); return paths;
}

QString firstError(QString *error, const QString &msg)
{
  if (error)
    *error = msg;
  return QString();
}

// 未决井链接 → 目标井 id（与导入同一判据的子集）：文件名主名优先，
// note「未匹配井名: X」兜底；恰好 1 候选才回，2+/0 不猜。
// 目标 (well, role) 已有已决主关联 → 回空（没有可补的位）。
QString resolvableWellTarget(DataCatalog *cat, const EntityAssetLink &l)
{
  const CatalogAsset a = cat->assetById(l.assetId);
  if (a.id.isEmpty())
    return QString();
  const CatalogVersion v = cat->currentVersion(l.assetId);

  QStringList names;
  const QString stem = QFileInfo(v.fileName.isEmpty() ? a.displayName : v.fileName)
                           .completeBaseName();
  if (!stem.isEmpty())
    names << stem;
  static const QString kUnmatched = QStringLiteral("未匹配井名: ");
  if (l.note.startsWith(kUnmatched))
    names << l.note.mid(kUnmatched.size()).split(QStringLiteral(", "));

  QString target;
  for (const QString &n : names)
  {
    if (n.trimmed().isEmpty())
      continue;
    const QStringList ids = cat->wellsMatchingName(n);
    if (ids.size() == 1)
    {
      target = ids.front();
      break;
    }
    if (!ids.isEmpty())
      break; // 2+ 候选仍不决——与导入同一判据，不猜
  }
  if (target.isEmpty())
    return QString();
  for (const EntityAssetLink &o : cat->linksForEntity(target))
    if (o.role == l.role && o.isPrimary && !o.unresolved)
      return QString();
  return target;
}

// 文本行对比的格式白名单（按小写扩展名）——其余格式如实标「不可文本对比」。
bool isTextLikeFormat(const QString &format)
{
  static const QSet<QString> kTextFormats = {
      QStringLiteral("las"),   QStringLiteral("dat"), QStringLiteral("txt"),
      QStringLiteral("csv"),   QStringLiteral("json"), QStringLiteral("geojson"),
      QStringLiteral("md"),    QStringLiteral("xml")};
  return kTextFormats.contains(format.toLower());
}

QStringList readTextLines(const QString &path)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return QStringList();
  const QByteArray raw = f.read(kMaxTextDiffBytes + 1);
  if (raw.size() > kMaxTextDiffBytes)
    return QStringList(); // 超限 = 不比（调用方按空串判不可比）
  QString text = QString::fromUtf8(raw);
  // 兼容 UTF-8 BOM
  if (text.startsWith(QChar(0xFEFF)))
    text.remove(0, 1);
  return text.split(QLatin1Char('\n'));
}

} // namespace

RollbackOutcome rollbackToVersion(DataCatalog *cat, const QString &projectDir,
                                  const QString &assetId,
                                  const QString &targetVersionId, QString *error)
{
  RollbackOutcome out;
  if (!cat || !cat->isOpen())
  {
    firstError(error, QStringLiteral("工程 catalog 未打开"));
    return out;
  }
  const CatalogVersion target = cat->versionById(targetVersionId);
  if (target.id.isEmpty())
  {
    firstError(error, QStringLiteral("目标版本不存在：%1").arg(targetVersionId));
    return out;
  }
  if (target.assetId != assetId)
  {
    firstError(error, QStringLiteral("目标版本 %1 不属于资产 %2")
                         .arg(targetVersionId, assetId));
    return out;
  }
  const CatalogVersion current = cat->currentVersion(assetId);
  if (current.id == target.id)
  {
    firstError(error, QStringLiteral("目标版本即当前版本（v%1），无需回滚")
                         .arg(current.versionNumber));
    return out;
  }

  const QString newVersionId = cat->nextVersionId();
  const int newNumber = (current.versionNumber > 0 ? current.versionNumber : 0) + 1;

  QString path = target.path;
  QString sha = target.sha256;

  if (target.managed)
  {
    const QString src = DataCatalog::resolvedVersionPath(projectDir, target);
    if (src.isEmpty() || !QFile::exists(src))
    {
      firstError(error, QStringLiteral("目标版本文件缺失：%1").arg(src));
      return out;
    }
    const QString rel = DataCatalog::managedPath(target.stage, assetId, newVersionId,
                                                 target.fileName);
    if (rel.isEmpty())
    {
      firstError(error, QStringLiteral("受管路径段非法：stage=%1 fileName=%2")
                            .arg(target.stage, target.fileName));
      return out;
    }
    const QString dst = QDir(projectDir).filePath(rel);
    QDir().mkpath(QFileInfo(dst).absolutePath());
    if (!QFile::copy(src, dst))
    {
      QFile::remove(dst); // 半截副本不留
      firstError(error, QStringLiteral("回滚拷贝失败：%1 → %2").arg(src, dst));
      return out;
    }
    // 拷后重算摘要：与目标留底不一致即失败——绝不回滚出被篡改的内容。
    QString serr;
    const QString actualSha = DataCatalog::sha256FileHex(dst, &serr);
    if (actualSha.isEmpty())
    {
      QFile::remove(dst);
      firstError(error, QStringLiteral("回滚副本摘要计算失败：%1").arg(serr));
      return out;
    }
    if (!target.sha256.isEmpty() && actualSha != target.sha256)
    {
      QFile::remove(dst);
      firstError(error, QStringLiteral("回滚副本与目标版本 SHA-256 不一致（%1 ≠ %2），已丢弃")
                       .arg(actualSha, target.sha256));
      return out;
    }
    sha = actualSha;
    QFile::setPermissions(dst, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                   QFileDevice::ReadGroup | QFileDevice::ReadOther);
    path = rel;
  }
  // 外链目标：新版本记录指向同一源路径 + 留底 SHA（零拷贝）。

  CatalogVersion nv;
  nv.id = newVersionId;
  nv.assetId = assetId;
  nv.stage = target.stage;
  nv.versionNumber = newNumber;
  nv.managed = target.managed;
  nv.path = path;
  nv.sourceUri = target.sourceUri;
  nv.sha256 = sha;
  nv.fileName = target.fileName;
  nv.parentVersionIds = QStringList{target.id};
  // 目标版本的语义元数据随字节一起复活（depthMd 深度锚/geojson 图例/
  // wellParseReport 等——丢了它们，井道按深度挂图、图例展示在回滚后失效）。
  // stale 标记除外：回滚就是把这些字节重新立为当前，带着「已被取代」标
  // 记会让 removeStaleVersions 视当前版本可清。
  nv.extra = target.extra;
  nv.extra.remove(QStringLiteral("stale"));
  nv.extra.remove(QStringLiteral("staleReason"));
  nv.extra.insert(QStringLiteral("rollbackOf"), target.id);

  DataCatalog::BatchSave batch(cat);
  QString aerr;
  if (!cat->addVersion(nv, &aerr))
  {
    if (target.managed)
      QFile::remove(QDir(projectDir).filePath(path));
    firstError(error, QStringLiteral("登记回滚版本失败：%1").arg(aerr));
    return out;
  }
  QString ferr;
  if (!batch.flush(&ferr))
  {
    // 落盘失败：catalog 内存已回滚到批次前；受管副本同步清掉。
    if (target.managed)
      QFile::remove(QDir(projectDir).filePath(path));
    firstError(error, QStringLiteral("回滚落盘失败（整批已回滚）：%1").arg(ferr));
    return out;
  }

  out.versionId = newVersionId;
  out.versionNumber = newNumber;
  out.sha256 = sha;
  return out;
}

VersionCompare compareVersions(const QString &projectDir, const CatalogVersion &a,
                               const CatalogVersion &b)
{
  VersionCompare c;
  c.shaKnown = !a.sha256.isEmpty() && !b.sha256.isEmpty();
  c.sameSha = c.shaKnown && a.sha256 == b.sha256;

  // 受管版本只认 resolvedVersionPath 的包含性结论：解析为空（绝对路径/
  // 反斜杠/不安全路径段/路径上任一段是符号链接/规范路径越出工程）即视为
  // 「不可读」——旧实现回落到原始 v.path，绝对路径会读出工程外文件、相对
  // 路径会相对进程 CWD 读到无关文件，绕过 purge/预览同一把包含性闸门。
  // 非受管（外链）版本 resolvedVersionPath 原样返回登记的 path（不做包含性
  // 检查，可能是相对路径），本函数对其行为不变。
  const QString absA = DataCatalog::resolvedVersionPath(projectDir, a);
  const QString absB = DataCatalog::resolvedVersionPath(projectDir, b);
  const QFileInfo fa(absA);
  const QFileInfo fb(absB);
  const bool hasA = !absA.isEmpty() && fa.exists();
  const bool hasB = !absB.isEmpty() && fb.exists();
  if (hasA)
    c.sizeA = fa.size();
  if (hasB)
    c.sizeB = fb.size();

  const auto addDiff = [&c](const QString &field, const QString &va, const QString &vb) {
    if (va != vb)
      c.fieldDiffs.append({field, va, vb});
  };
  addDiff(QStringLiteral("stage"), a.stage, b.stage);
  addDiff(QStringLiteral("managed"), a.managed ? QStringLiteral("true")
                                               : QStringLiteral("false"),
          b.managed ? QStringLiteral("true") : QStringLiteral("false"));
  addDiff(QStringLiteral("sourceUri"), a.sourceUri, b.sourceUri);
  addDiff(QStringLiteral("fileName"), a.fileName, b.fileName);
  addDiff(QStringLiteral("parentVersionIds"), a.parentVersionIds.join(QLatin1Char(',')),
          b.parentVersionIds.join(QLatin1Char(',')));

  // 文本行级对比：双侧同文本格式、都在、都 ≤ 上限。
  const QString fmtA = QFileInfo(a.fileName).suffix().toLower();
  const QString fmtB = QFileInfo(b.fileName).suffix().toLower();
  if (isTextLikeFormat(fmtA) && isTextLikeFormat(fmtB) && hasA && hasB &&
      fa.size() <= kMaxTextDiffBytes && fb.size() <= kMaxTextDiffBytes)
  {
    const QStringList la = readTextLines(absA);
    const QStringList lb = readTextLines(absB);
    if (!la.isEmpty() || !lb.isEmpty())
    {
      c.textCompared = true;
      int p = 0;
      while (p < la.size() && p < lb.size() && la.at(p) == lb.at(p))
        ++p;
      int sa = la.size(), sb = lb.size();
      int s = 0;
      while (s < la.size() - p && s < lb.size() - p &&
             la.at(la.size() - 1 - s) == lb.at(lb.size() - 1 - s))
        ++s;
      c.linesA = la.size() - p - s;
      c.linesB = lb.size() - p - s;
      // 样本预算两侧分摊：旧实现先把 40 条全给 A 侧，A 侧差异块 ≥40 行时
      // B 侧（新内容）一条都看不到。各取至多一半，一侧不足时余量让给另一侧；
      // 总数仍封顶 kMaxSample，输出顺序仍是先「-」后「+」。
      static constexpr int kMaxSample = 40;
      const int blockA = qMax(0, sa - s - p), blockB = qMax(0, sb - s - p);
      int takeA = qMin(blockA, kMaxSample / 2);
      const int takeB = qMin(blockB, kMaxSample - takeA);
      takeA = qMin(blockA, kMaxSample - takeB);
      for (int i = p; i < p + takeA; ++i)
        c.differingLines << QStringLiteral("- ") + la.at(i);
      for (int i = p; i < p + takeB; ++i)
        c.differingLines << QStringLiteral("+ ") + lb.at(i);
    }
  }
  return c;
}

PurgeOutcome purgeAssets(DataCatalog *cat, const QString &projectDir,
                         const QStringList &assetIds, bool atomic, bool deferFiles)
{
  PurgeOutcome out;
  if (!cat || !cat->isOpen())
  {
    out.failedAssets << QStringLiteral("工程 catalog 未打开");
    return out;
  }

  // 预采集：removeAsset 提交后版本行就没了——文件路径/大小必须先拿到手。
  struct Target
  {
    QString assetId;
    QStringList managedFiles; // 绝对路径
  };
  QVector<Target> targets;
  for (const QString &id : assetIds)
  {
    const CatalogAsset a = cat->assetById(id);
    if (a.id.isEmpty())
    {
      out.failedAssets << QStringLiteral("%1: 资产不存在").arg(id);
      continue;
    }
    Target t;
    t.assetId = id;
    for (const CatalogVersion &v : cat->versionsForAsset(id))
    {
      if (!v.managed)
        continue; // 外链源在工程外，永不删
      t.managedFiles << versionFiles(projectDir, v);
    }
    targets.append(t);
  }
  if (targets.isEmpty())
    return out;
  if (atomic && !out.failedAssets.isEmpty())
    return out;

  // 单事务：全部 removeAsset 并入一个 BatchSave，flush 失败整批回滚零文件动作。
  DataCatalog::BatchSave batch(cat);
  QStringList doomed; // flush 成功后才动文件的 id
  if (atomic) {
    QStringList ids;
    for (const auto &target : targets) ids << target.assetId;
    QString error;
    if (!cat->removeAssets(ids, &error)) {
      out.failedAssets << error;
      return out;
    }
    doomed = ids;
  }
  else for (const Target &t : targets)
  {
    QString err;
    if (cat->removeAsset(t.assetId, &err))
      doomed << t.assetId;
    else
      out.failedAssets << QStringLiteral("%1: %2")
                              .arg(cat->assetById(t.assetId).displayName.isEmpty()
                                       ? t.assetId
                                       : cat->assetById(t.assetId).displayName, err);
  }
  if (doomed.isEmpty())
    return out; // 内存态批内回滚由 removeAsset 自身失败路径 + 析构保证
  QString ferr;
  if (!batch.flush(&ferr))
  {
    // 落盘失败：endBatch 已把内存还原到批前——所有入选资产按失败列报。
    for (const Target &t : targets)
      out.failedAssets << QStringLiteral("%1: 落盘失败（整批回滚）")
                              .arg(t.assetId);
    out.failedAssets << ferr;
    return out;
  }

  QStringList paths;
  for (const Target &t : targets)
    if (doomed.contains(t.assetId)) paths.append(t.managedFiles);
  if (deferFiles) {
    out.pendingFiles = paths;
    out.purgedAssetIds = doomed;
    return out;
  }
  const auto files = purgeManagedFiles(projectDir, paths, cat->versions());
  out.bytesFreed = files.bytesFreed;
  out.leftoverFiles = files.leftoverFiles;
  out.purgedAssetIds = doomed;
  return out;
}

PurgeOutcome purgeManagedFiles(const QString &projectDir, const QStringList &paths,
                               const QVector<CatalogVersion> &retained,
                               const QVector<PurgeFileExpectation> &expected)
{
  PurgeOutcome out;
  QSet<QString> referenced, seen, prunedDirs;
  for (const auto &v : retained) {
    auto files = versionFiles(projectDir, v);
    // Legacy/external aliases may lack bundle metadata but still depend on
    // the same SHP/DBF/SHX family. Protect them as one dataset as well.
    const auto primary = DataCatalog::resolvedVersionPath(projectDir, v);
    if (QFileInfo(primary).suffix().compare("shp", Qt::CaseInsensitive) == 0)
      files += PaleoMapFiles::members(primary, nullptr);
    for (const auto &path : files)
      referenced.insert(PathCanon::canonicalize(path, v.managed ? projectDir : QString()));
  }
  QHash<QString, PurgeFileExpectation> expectations;
  for (const auto &f : expected) expectations.insert(f.path, f);
  for (const auto &path : paths) {
    CatalogVersion probe; probe.path = QDir(projectDir).relativeFilePath(path);
    const QString safe = DataCatalog::resolvedVersionPath(projectDir, probe);
    const QString canonical = PathCanon::canonicalize(path);
    if (safe.isEmpty()) { out.leftoverFiles << path; continue; }
    if (seen.contains(canonical) || referenced.contains(canonical)) continue;
    seen.insert(canonical);
    const QFileInfo fi(safe);
    if (!fi.exists()) continue;
    const qint64 bytes = fi.size();
    if (expectations.contains(path)) {
      const auto expectedFile = expectations.value(path);
      if (bytes != expectedFile.sizeBytes || fi.lastModified() != expectedFile.modified) {
        out.leftoverFiles << safe; continue;
      }
    }
    QFile::setPermissions(safe, QFile::permissions(safe) | QFileDevice::WriteOwner);
    if (QFile::remove(safe)) out.bytesFreed += bytes;
    else out.leftoverFiles << safe;
    prunedDirs.insert(fi.absolutePath());
  }
  for (const auto &dir : prunedDirs) {
    QString d = dir;
    for (int i = 0; i < 2 && !d.isEmpty(); ++i) {
      if (!QDir(d).rmdir(QStringLiteral("."))) break;
      d = QFileInfo(d).absolutePath();
    }
  }
  return out;
}

QVector<PendingProposal> proposablePendingLinks(DataCatalog *cat)
{
  QVector<PendingProposal> out;
  if (!cat)
    return out;
  const QVector<EntityAssetLink> links = cat->links();
  for (int i = 0; i < links.size(); ++i)
  {
    const EntityAssetLink &l = links.at(i);
    if (!l.unresolved || l.entityType != QLatin1String("well"))
      continue;
    const QString target = resolvableWellTarget(cat, l);
    if (target.isEmpty())
      continue;

    const CatalogAsset a = cat->assetById(l.assetId);
    PendingProposal p;
    p.linkIndex = i;
    p.assetId = l.assetId;
    p.assetName = a.displayName;
    p.wellId = target;
    const CatalogEntity well = cat->entityById(target);
    p.wellName = well.name.isEmpty() ? target : well.name;
    const CatalogVersion v = cat->currentVersion(l.assetId);
    p.sourceName = QFileInfo(v.fileName.isEmpty() ? a.displayName : v.fileName)
                       .completeBaseName();
    out.append(p);
  }
  return out;
}

int applyPendingResolutions(DataCatalog *cat, const QVector<int> &linkIndexes,
                            QString *error)
{
  if (!cat || !cat->isOpen())
  {
    firstError(error, QStringLiteral("工程 catalog 未打开"));
    return 0;
  }
  DataCatalog::BatchSave batch(cat);
  int attached = 0;
  for (const int idx : linkIndexes)
  {
    const QVector<EntityAssetLink> links = cat->links(); // attachLink 不增删行，序稳定
    if (idx < 0 || idx >= links.size())
      continue;
    const EntityAssetLink &l = links.at(idx);
    if (!l.unresolved)
      continue;
    // 按执行时刻的 catalog 事实重判（提议与执行之间可能隔了用户操作）。
    const QString target = resolvableWellTarget(cat, l);
    if (target.isEmpty())
      continue;
    QString aerr;
    if (!cat->attachLink(idx, target, &aerr))
    {
      qWarning() << "applyPendingResolutions: attachLink failed" << aerr;
      continue;
    }
    ++attached;
  }
  if (attached == 0)
    return 0;
  QString ferr;
  if (!batch.flush(&ferr))
  {
    firstError(error, QStringLiteral("归位落盘失败（整批回滚）：%1").arg(ferr));
    return 0;
  }
  return attached;
}

bool removeAssetsKeepFiles(DataCatalog *cat, const QStringList &assetIds,
                           QString *error)
{
  if (!cat)
  {
    firstError(error, QStringLiteral("catalog is null"));
    return false;
  }
  // 文件一律不动——removeAssets 本就只删索引（含血缘/staging 保护），
  // 这里只是给「不接 purgeManagedFiles」的调用面一个显式名字。
  return cat->removeAssets(assetIds, error);
}

QVector<OrphanManagedDir> scanOrphanManagedDirs(const QString &projectDir,
                                                const DataCatalog *cat)
{
  QVector<OrphanManagedDir> out;
  if (projectDir.isEmpty() || !cat)
    return out;
  QSet<QString> known;
  for (const CatalogAsset &a : cat->assets())
    known.insert(a.id);
  // stage 目录名大小写不敏感（catalog stage 值是 RAW/DERIVED 大写、
  // 文档口径是小写 raw/derived——以盘上实际名为准，归一化后匹配）。
  const QSet<QString> stages{QStringLiteral("raw"), QStringLiteral("derived"),
                             QStringLiteral("intermediate"), QStringLiteral("output")};
  const QDir artifactsDir(projectDir + QStringLiteral("/artifacts"));
  if (!artifactsDir.exists())
    return out;
  const QStringList stageEntries =
      artifactsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString &stageEntry : stageEntries)
  {
    const QString stageLower = stageEntry.toLower();
    if (!stages.contains(stageLower))
      continue;
    const QDir stageDir(artifactsDir.absoluteFilePath(stageEntry));
    const QStringList sub = stageDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &assetId : sub)
    {
      if (known.contains(assetId))
        continue;
      OrphanManagedDir d;
      d.relPath = stageLower + QLatin1Char('/') + assetId;
      d.absPath = stageDir.absoluteFilePath(assetId);
      QDirIterator it(d.absPath, QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext())
      {
        it.next();
        d.bytes += static_cast<qint64>(it.fileInfo().size());
        ++d.fileCount;
      }
      out << d;
    }
  }
  return out;
}

} // namespace paleo::assetops
