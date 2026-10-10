// 层：数据
#include "datacatalog.h"
#include "../metadata/storeerrors_internal.h"

#include <QSet>

namespace
{
using paleo::store_detail::setError;
} // namespace

// 派生族（方向 99 拆分）：parentVersionIds 反查下游闭包（BFS）与
// staleness-lite 下游失效标记（B 包）。血缘邻接走 D5.4 childrenByParent。

QVector<CatalogVersion> DataCatalog::downstreamClosure(const QString &versionId) const
{
  QVector<CatalogVersion> out;
  if (versionId.isEmpty())
    return out;
  // BFS：邻接表反查（D5.4 childrenByParent）——不再每层全表扫 m_versions。
  // 结果序 = BFS 发现序（frontier 序 × 子版本行序），与旧「全表扫描 × 表序」
  // 逐项一致（子版本按行序入边）。seen 先放种子——环（A→B→A）里种子不作为
  // 「自己的下游」进结果，也保证遍历终止。
  QSet<QString> seen;
  seen.insert(versionId);
  QStringList frontier{versionId};
  for (int head = 0; head < frontier.size(); ++head)
  {
    // 值拷贝：frontier.append 可能重分配 QList 存储，引用会悬垂。
    const QString cur = frontier.at(head);
    for (const QString &childId : m_idx.childVersionIds(cur))
    {
      if (seen.contains(childId))
        continue;
      const int row = m_idx.versionRow(childId);
      if (row < 0)
        continue;
      seen.insert(childId);
      frontier.append(childId);
      out.append(m_versions.at(row));
    }
  }
  return out;
}

int DataCatalog::markStaleDownstreamOf(const QString &versionId, const QString &reason,
                                       QVector<QPair<int, CatalogVersion>> *undo)
{
  const QVector<CatalogVersion> downstream = downstreamClosure(versionId);
  int changed = 0;
  for (const CatalogVersion &d : downstream)
  {
    // staleness-lite：只标 DERIVED 产物。闭包里的 INTERMEDIATE/OUTPUT 等
    // 参与溯源穿透（DERIVED 的祖先可以是任意阶段），但自身不记 stale。
    if (d.stage != QLatin1String("DERIVED"))
      continue;
    // WP2：行号走邻接索引 O(1)——旧实现每次全表建 id→row QHash，是
    // addVersion 路径隐藏的 O(N)/调用（N 次导入即 O(N²)）。
    const int i = m_idx.versionRow(d.id);
    if (i < 0)
      continue; // 闭包快照自洽——防御性跳过
    CatalogVersion &m = m_versions[i];
    if (m.extra.value(QStringLiteral("stale")).toBool() &&
        m.extra.value(QStringLiteral("staleReason")).toString() == reason)
      continue; // 同一标记已在——不算变更（幂等，不空涨 revision）
    if (undo)
      undo->append({i, m});
    m.extra.insert(QStringLiteral("stale"), true);
    m.extra.insert(QStringLiteral("staleReason"), reason);
    m_dirtyVersions.insert(m.id);
    ++changed;
  }
  return changed;
}

bool DataCatalog::markDownstreamStale(const QString &versionId, const QString &reason,
                                      QString *error)
{
  if (!checkWriteThread("markDownstreamStale", error))
    return false;
  if (!ensureOpen(error))
    return false;
  if (versionId.isEmpty())
  {
    setError(error, QStringLiteral("cannot mark downstream of an empty version id"));
    return false;
  }
  if (versionById(versionId).id.isEmpty())
  {
    setError(error,
             QStringLiteral("cannot mark downstream of unknown version: %1").arg(versionId));
    return false;
  }
  const QString why =
      reason.isEmpty() ? QStringLiteral("上游版本源已失效") : reason;
  // WP2：全表快照换 undo 栈（O(命中行)）——落盘失败逆序还原，纪律不变。
  QVector<QPair<int, CatalogVersion>> undo;
  if (markStaleDownstreamOf(versionId, why, &undo) == 0)
    return true; // 无下游或标记未变——不落盘、不空涨 revision
  if (save(error))
  {
    CatalogOp op;
    op.kind = CatalogOp::Kind::MarkDownstreamStale;
    op.id = versionId;
    op.reason = reason;
    recordOp(std::move(op));
    return true;
  }
  for (int i = undo.size() - 1; i >= 0; --i)
    m_versions[undo[i].first] = undo[i].second; // 落盘失败回滚内存
  return false;
}
