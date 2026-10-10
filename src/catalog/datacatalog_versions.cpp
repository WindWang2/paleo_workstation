// 层：数据
#include "datacatalog.h"
#include "../metadata/storeerrors_internal.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>

namespace
{
using paleo::store_detail::setError;

// updateVersionExtra 的 #history 审计保留条数（FIFO）——足够覆盖「改错
// 再改回」的常见往返，又不让长期反复编辑的 extra 无界膨胀。
constexpr int kExtraHistoryKeep = 8;
} // namespace

// 版本族（方向 99 拆分）：addVersion（含 supersede 级联标 stale）、
// 版本查询、extra 就地更新（方向 79 锚深编辑 + 旧 map 合并面）、
// 治理删除（stale DERIVED）、受管路径解析/校验与 SHA-256 流式摘要、
// 受 "ver-N" id 分配。

bool DataCatalog::addVersion(const CatalogVersion &v, QString *error)
{
  if (!checkWriteThread("addVersion", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (v.id.isEmpty() || v.assetId.isEmpty())
  {
    setError(error, QStringLiteral("version id or asset id is empty"));
    return false;
  }
  if (!versionById(v.id).id.isEmpty())
  {
    setError(error, QStringLiteral("duplicate version id: %1").arg(v.id));
    return false;
  }
  if (assetById(v.assetId).id.isEmpty())
  {
    setError(error, QStringLiteral("version references unknown asset: %1").arg(v.assetId));
    return false;
  }
  // §3：fileName 与受管 path 的每一段都必须是合法路径段——catalog 不落坏段。
  // （外链 path 是文件系统绝对路径，含分隔符属正常，不查；空 path 表示未落位，
  // 交给调用方兜底。）
  if (!v.fileName.isEmpty() && !isSafePathSegment(v.fileName))
  {
    setError(error, QStringLiteral("unsafe path segment in version file name: %1").arg(v.fileName));
    return false;
  }
  if (!v.stage.isEmpty() && !isSafePathSegment(v.stage))
  {
    setError(error, QStringLiteral("unsafe path segment in version stage: %1").arg(v.stage));
    return false;
  }
  if (v.managed && !v.path.isEmpty())
  {
    for (const QString &seg : v.path.split(QLatin1Char('/')))
      if (!isSafePathSegment(seg))
      {
        setError(error, QStringLiteral("unsafe managed path segment: %1").arg(seg));
        return false;
      }
    if (resolvedVersionPath(m_dir, v).isEmpty())
    {
      setError(error, QStringLiteral("unsafe managed path: %1").arg(v.path));
      return false;
    }
  }
  // T17：显式给的 "ver-N" 也推进序号——不然 addVersion("ver-9") 之后
  // nextVersionId() 还会发 ver-9（被 dup 检查挡下报错）而不是发 ver-10。
  {
    bool ok = false;
    const int n = v.id.startsWith(QStringLiteral("ver-")) ? v.id.mid(4).toInt(&ok) : 0;
    if (ok && n > 0)
      m_versionSeq = qMax(m_versionSeq, n);
  }
  // WP2（catalog 写路径线性化）：不再全表快照 previousVersions——那会让
  // 每次 append 触发 QVector COW detach 的 O(N) 深拷贝（N 次导入即 O(N²)，
  // 100k 夹具实测 ~950s 的大头）。改精确 undo：新增行 removeLast 回退，
  // staleness 标记经 markStaleDownstreamOf 的 undo 栈逆序还原——save 失败
  // 仍是「版本与 stale 标记同进同退」的同一纪律，只是回退成本从 O(N)/次
  // 降到 O(undo)/次（undo 大小 = 下游闭包命中数，通常个位数）。
  m_versions.append(v);
  m_idx.versionAdded(m_versions.size() - 1, v); // D5.2
  // B 包 staleness-lite：新版本入库 = 同资产 versionNumber 更低的旧版本被
  // 取代（supersede）——其下游闭包中的 DERIVED 版本输入失效，随本次
  // addVersion 同一原子写落 extra["stale"]/["staleReason"]，不二次落盘。
  QStringList superseded;
  for (int r : m_idx.versionRowsForAsset(v.assetId)) // D5.1 O(命中集) 非全表
    if (m_versions.at(r).versionNumber < v.versionNumber)
      superseded.append(m_versions.at(r).id);
  QVector<QPair<int, CatalogVersion>> staleUndo;
  for (const QString &pid : superseded)
    markStaleDownstreamOf(
        pid, QStringLiteral("上游版本 %1 已被同资产新版本 %2 取代").arg(pid, v.id),
        &staleUndo);
  m_dirtyVersions.insert(v.id);
  if (save(error))
  {
    CatalogOp op;
    op.kind = CatalogOp::Kind::AddVersion;
    op.version = v;
    recordOp(std::move(op));
    return true;
  }
  for (int i = staleUndo.size() - 1; i >= 0; --i) // 逆序：同行的最早原值最后落
    m_versions[staleUndo[i].first] = staleUndo[i].second;
  m_versions.removeLast();
  m_idx.versionsMutated(m_versions);
  return false;
}

// 方向 79：版本 extra 就地更新（锚深后补编辑）。就地 vs 新版本的定案
// 与审计/撤销判据见 datacatalog.h 契约注与 ledger——不 fork 血统、不动
// versionNumber/path/sha256；改前值进 #history（FIFO ≤8）、来源标 #source。
// history 条目 {v, had, at}：had=false 表示此前无值（补锚场景），v 省略——
// 无效 QVariant 转 JSON 会整键消失，不用 null 冒充。
bool DataCatalog::updateVersionExtra(const QString &versionId, const QString &key,
                                     const QVariant &value, QString *error)
{
  if (!checkWriteThread("updateVersionExtra", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (key.isEmpty() || key.contains(QLatin1Char('#')))
  {
    setError(error, QStringLiteral("extra key 不能为空、不能含 '#'（#history/#source 是审计保留字）"));
    return false;
  }
  const int row = m_idx.versionRow(versionId);
  if (row < 0 || row >= m_versions.size() || m_versions.at(row).id != versionId)
  {
    setError(error, QStringLiteral("unknown version id: %1").arg(versionId));
    return false;
  }
  const CatalogVersion undo = m_versions.at(row);
  CatalogVersion next = undo;
  const QVariant old = next.extra.value(key);
  // 真 no-op：同值重写（含清不存在的键）不动 catalog——不涨 revision、
  // 不留 history 噪声（用户打开编辑器没改值点确定，不是一次编辑）。
  const bool sameValue =
      value.isValid() == old.isValid() &&
      (!value.isValid() || value == old);
  if (sameValue)
    return true;
  QVariantList history = next.extra.value(QStringLiteral("%1#history").arg(key)).toList();
  QVariantMap entry;
  if (old.isValid())
    entry.insert(QStringLiteral("v"), old);
  entry.insert(QStringLiteral("had"), old.isValid());
  entry.insert(QStringLiteral("at"),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  history.append(entry);
  while (history.size() > kExtraHistoryKeep)
    history.removeFirst();
  next.extra.insert(QStringLiteral("%1#history").arg(key), history);
  if (value.isValid())
  {
    next.extra.insert(key, value);
    next.extra.insert(QStringLiteral("%1#source").arg(key),
                      QStringLiteral("manual"));
  }
  else
  {
    next.extra.remove(key);
    next.extra.remove(QStringLiteral("%1#source").arg(key));
  }
  m_versions[row] = next;
  m_dirtyVersions.insert(versionId);
  if (save(error))
  {
    CatalogOp op;
    op.kind = CatalogOp::Kind::UpdateVersionExtra;
    op.id = versionId;
    op.extraKey = key;
    op.extraValue = value;
    recordOp(std::move(op));
    return true;
  }
  m_versions[row] = undo; // save 失败：内存还原，盘上不动
  return false;
}

CatalogVersion DataCatalog::versionBySha256(const QString &sha256) const
{
  noteRead("versionBySha256");
  if (sha256.isEmpty())
    return CatalogVersion();
  // WP2：sha 命中行集走索引（旧实现线性全表扫——导入 dedup 每行调一次，
  // N 文件导入即 O(N²)）。行集升序＝表序——「第一个匹配」语义逐字节不变；
  // 命中后照旧复核文件在且字节一致（死路径旧记录不冒充命中）。
  const QVector<int> rows = m_idx.versionRowsForSha(sha256.toLower());
  for (int r : rows)
  {
    const CatalogVersion &v = m_versions.at(r);
    const QString path = versionFilePath(v); // staging：同批新增受管版本在暂存根
    if (path.isEmpty() || !QFileInfo(path).isFile()) continue;
    if (sha256FileHex(path).compare(sha256, Qt::CaseInsensitive) == 0)
      return v;
  }
  return CatalogVersion();
}

bool DataCatalog::removeStaleVersions(const QStringList &versionIds, QString *error)
{
  if (!checkWriteThread("removeStaleVersions", error) || !ensureOpen(error))
    return false;
  if (m_staging || versionIds.isEmpty()) {
    setError(error, QStringLiteral("stale removal requires a live catalog and selected versions"));
    return false;
  }
  const QSet<QString> selected(versionIds.begin(), versionIds.end());
  for (const QString &id : selected) {
    const CatalogVersion v = versionById(id);
    if (v.id.isEmpty() || v.stage != QLatin1String("DERIVED") || !v.extra.value(QStringLiteral("stale")).toBool()) {
      setError(error, QStringLiteral("version is not stale DERIVED: %1").arg(id));
      return false;
    }
    for (const QString &child : m_idx.childVersionIds(id))
      if (!selected.contains(child)) {
        setError(error, QStringLiteral("version %1 is referenced by retained version %2").arg(id, child));
        return false;
      }
  }
  const auto previous = m_versions;
  const auto previousRemoved = m_removedVersions;
  m_versions.erase(std::remove_if(m_versions.begin(), m_versions.end(),
    [&](const auto &v) { return selected.contains(v.id); }), m_versions.end());
  m_removedVersions.unite(selected);
  m_idx.rebuild(m_entities, m_assets, m_versions, m_links);
  if (save(error)) { ++m_mutationSeq; return true; }
  m_versions = previous;
  m_removedVersions = previousRemoved;
  m_idx.rebuild(m_entities, m_assets, m_versions, m_links);
  return false;
}

bool DataCatalog::updateVersionExtra(const QString &versionId, const QVariantMap &extra, QString *error)
{
  if (!checkWriteThread("updateVersionExtra", error) || !ensureOpen(error))
    return false;
  if (versionId.isEmpty())
  {
    setError(error, QStringLiteral("cannot update extra of an empty version id"));
    return false;
  }
  const int row = m_idx.versionRow(versionId);
  if (row < 0 || row >= m_versions.size())
  {
    setError(error, QStringLiteral("unknown version id: %1").arg(versionId));
    return false;
  }
  for (auto it = extra.begin(); it != extra.end(); ++it)
    m_versions[row].extra.insert(it.key(), it.value());
  m_dirtyVersions.insert(versionId);
  if (save(error))
  {
    ++m_mutationSeq;
    return true;
  }
  return false;
}

bool DataCatalog::updateAssetExtra(const QString &assetId, const QVariantMap &extra, QString *error)
{
  if (!checkWriteThread("updateAssetExtra", error) || !ensureOpen(error))
    return false;
  if (assetId.isEmpty())
  {
    setError(error, QStringLiteral("cannot update extra of an empty asset id"));
    return false;
  }
  const CatalogVersion v = currentVersion(assetId);
  if (v.id.isEmpty())
  {
    setError(error, QStringLiteral("asset has no current version: %1").arg(assetId));
    return false;
  }
  return updateVersionExtra(v.id, extra, error);
}

QString DataCatalog::resolvedVersionPath(const QString &projectDir, const CatalogVersion &version)
{
  if (version.path.isEmpty()) return QString();
  if (!version.managed) return version.path;
  if (QDir::isAbsolutePath(version.path) || version.path.contains(QLatin1Char('\\')))
    return QString();
  const QStringList segments = version.path.split(QLatin1Char('/'));
  for (const QString &segment : segments)
    if (!isSafePathSegment(segment)) return QString();

  const QString root = QFileInfo(projectDir).canonicalFilePath();
  if (root.isEmpty()) return QString();
  QString current = root;
  for (const QString &segment : segments)
  {
    current = QDir(current).filePath(segment);
    const QFileInfo info(current);
    if (info.isSymbolicLink()) return QString();
    if (info.exists())
    {
      // canonicalFilePath() 恒以 '/' 作分隔符（含 Windows）；QDir::separator()
      // 在 Windows 是 '\'，拼进前缀会让 startsWith 永假——曾致全部受管导入
      // 在 Windows 报 "unsafe managed path/destination"。
      const QString canonical = info.canonicalFilePath();
      if (!canonical.startsWith(root + QLatin1Char('/'))) return QString();
    }
  }
  return current;
}

QString DataCatalog::sha256FileHex(const QString &path, QString *error, const std::function<bool()> &cancelled)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    setError(error, QStringLiteral("cannot read %1").arg(path));
    return QString();
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  // 64KB chunks: a 1MB stack buffer overflows the default Windows thread
  // stack when this runs on the QTest main thread.
  char buf[64 << 10];
  qint64 n = 0;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    if (cancelled && cancelled()) {
      setError(error, QStringLiteral("SHA verification cancelled"));
      return QString();
    }
    hash.addData(QByteArrayView(buf, static_cast<qsizetype>(n)));
  }
  if (n < 0)
  {
    setError(error, QStringLiteral("read error on %1").arg(path));
    return QString();
  }
  return QString::fromLatin1(hash.result().toHex());
}

bool DataCatalog::verifyExternalVersionSha(const CatalogVersion &v, QString *error) const
{
  // 受管版本（自己写入的副本）与未留底的外链（旧 catalog）无从校验。
  if (v.managed || v.sha256.isEmpty())
    return true;
  QString herr;
  const QString current = sha256FileHex(v.path, &herr); // 外链 path 是绝对路径
  if (current.isEmpty())
  {
    setError(error, herr.isEmpty() ? QStringLiteral("cannot read %1").arg(v.path) : herr);
    return false;
  }
  if (current.compare(v.sha256, Qt::CaseInsensitive) != 0)
  {
    setError(error, QStringLiteral("源文件与入库时的 SHA-256 不一致"));
    return false;
  }
  return true;
}

bool DataCatalog::isSafePathSegment(const QString &segment)
{
  // §3：一段路径拒绝空段、"."、任何含 ".." 的段、斜杠/反斜杠、NUL 与控制字符
  //（含换行、回车、Tab、DEL）。
  if (segment.isEmpty() || segment == QLatin1Char('.') ||
      segment.contains(QLatin1String("..")))
    return false;
  for (const QChar c : segment)
  {
    if (c == QLatin1Char('/') || c == QLatin1Char('\\'))
      return false;
    const ushort u = c.unicode();
    if (u < 0x20 || u == 0x7F)
      return false;
  }
  return true;
}

QVector<CatalogVersion> DataCatalog::versionsForAsset(const QString &assetId) const
{
  noteRead("versionsForAsset");
  QVector<CatalogVersion> out;
  const QVector<int> rows = m_idx.versionRowsForAsset(assetId); // D5.1 O(1)+收集
  out.reserve(rows.size());
  for (int r : rows)
    out.append(m_versions.at(r));
  return out;
}

CatalogVersion DataCatalog::versionById(const QString &id) const
{
  noteRead("versionById");
  const int row = m_idx.versionRow(id); // D5.1 O(1)
  return row >= 0 ? m_versions.at(row) : CatalogVersion();
}

CatalogVersion DataCatalog::currentVersion(const QString &assetId) const
{
  noteRead("currentVersion");
  CatalogVersion best;
  const QVector<int> rows = m_idx.versionRowsForAsset(assetId); // D5.1
  for (int r : rows)
    if (m_versions.at(r).versionNumber >= best.versionNumber)
      best = m_versions.at(r);
  return best;
}

QString DataCatalog::managedPath(const QString &stage, const QString &assetId,
                                 const QString &versionId, const QString &fileName)
{
  // §3：任一段不是合法路径段就回空串——catalog 拒绝产出坏路径。
  if (!isSafePathSegment(stage) || !isSafePathSegment(assetId) ||
      !isSafePathSegment(versionId) || !isSafePathSegment(fileName))
    return QString();
  return QStringLiteral("%1/%2/%3/%4").arg(stage.toLower(), assetId, versionId, fileName);
}

QString DataCatalog::nextVersionId()
{
  noteRead("nextVersionId");
  ++m_mutationSeq; // 分配 id 也是 owner 侧状态变化（提交基线要看见）
  QString id;
  do { id = QStringLiteral("ver-%1").arg(++m_versionSeq); }
  while (!versionById(id).id.isEmpty());
  return id;
}
