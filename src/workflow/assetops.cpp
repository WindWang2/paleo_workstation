// 层：功能
#include "assetops.h"

#include "../catalog/datacatalog.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QSet>

namespace paleo::assetops
{

namespace
{

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

  const QString absA = DataCatalog::resolvedVersionPath(projectDir, a);
  const QString absB = DataCatalog::resolvedVersionPath(projectDir, b);
  QFileInfo fa(absA.isEmpty() ? a.path : absA);
  QFileInfo fb(absB.isEmpty() ? b.path : absB);
  if (fa.exists())
    c.sizeA = fa.size();
  if (fb.exists())
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
  if (isTextLikeFormat(fmtA) && isTextLikeFormat(fmtB) && fa.exists() && fb.exists() &&
      fa.size() <= kMaxTextDiffBytes && fb.size() <= kMaxTextDiffBytes)
  {
    const QStringList la = readTextLines(absA.isEmpty() ? a.path : absA);
    const QStringList lb = readTextLines(absB.isEmpty() ? b.path : absB);
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
      static constexpr int kMaxSample = 40;
      for (int i = p; i < sa - s && c.differingLines.size() < kMaxSample; ++i)
        c.differingLines << QStringLiteral("- ") + la.at(i);
      for (int i = p; i < sb - s && c.differingLines.size() < kMaxSample; ++i)
        c.differingLines << QStringLiteral("+ ") + lb.at(i);
    }
  }
  return c;
}

PurgeOutcome purgeAssets(DataCatalog *cat, const QString &projectDir,
                         const QStringList &assetIds)
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
      const QString abs = DataCatalog::resolvedVersionPath(projectDir, v);
      if (!abs.isEmpty())
        t.managedFiles << abs;
    }
    targets.append(t);
  }
  if (targets.isEmpty())
    return out;

  // 单事务：全部 removeAsset 并入一个 BatchSave，flush 失败整批回滚零文件动作。
  DataCatalog::BatchSave batch(cat);
  QStringList doomed; // flush 成功后才动文件的 id
  for (const Target &t : targets)
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

  // catalog 已提交——现在清磁盘（best-effort，残留如实报）。
  QSet<QString> prunedDirs;
  for (const Target &t : targets)
  {
    if (!doomed.contains(t.assetId))
      continue;
    for (const QString &abs : t.managedFiles)
    {
      if (!QFile::exists(abs))
        continue;
      const qint64 sz = QFileInfo(abs).size();
      if (QFile::remove(abs))
        out.bytesFreed += sz;
      else
        out.leftoverFiles << abs;
      prunedDirs.insert(QFileInfo(abs).absolutePath());
    }
    out.purgedAssetIds << t.assetId; // catalog 行已删；文件残留另列
  }
  // 版本目录 / 资产目录：只删空目录（rmdir 对非空目录失败即停，安全）。
  for (const QString &dir : prunedDirs)
  {
    QString d = dir;
    for (int i = 0; i < 2 && !d.isEmpty(); ++i) // {version} 层 + {asset} 层
    {
      if (!QDir(d).rmdir(QStringLiteral(".")))
        break;
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

} // namespace paleo::assetops
