// 层：视图
#include "editsession.h"

#include <QDateTime>

#include <cmath>

#include <algorithm>

namespace WellComposite
{

EditSession::EditSession(ComprehensiveWellData workingCopy, QObject *parent)
  : QObject(parent), m_doc(std::move(workingCopy)), m_stack(new EditStack(this))
{
  m_sourceId = m_doc.wellName;
  connect(m_stack.get(), &EditStack::stackChanged, this, &EditSession::documentChanged);
}

void EditSession::resetDocument(const ComprehensiveWellData &doc)
{
  m_doc = doc;
  m_stack->clear();
  m_audit.clear();
  m_sourceId = m_doc.wellName;
  emit documentChanged();
}

void EditSession::markSaved()
{
  m_stack->markSaved();
}

void EditSession::setReadOnly(bool readOnly, const QString &reason)
{
  if (m_readOnly == readOnly)
    return;
  m_readOnly = readOnly;
  m_readOnlyReason = readOnly ? reason : QString();
  emit readOnlyChanged(m_readOnly);
}

void EditSession::setSourceMtime(qint64 mtimeMs)
{
  m_sourceMtimeMs = mtimeMs;
}

bool EditSession::sourceChanged(qint64 currentMtimeMs) const
{
  // mtime 未初始化（0）视为无源跟踪
  if (m_sourceMtimeMs <= 0)
    return false;
  return currentMtimeMs != m_sourceMtimeMs;
}

void EditSession::checkSourceConflict(qint64 currentMtimeMs)
{
  if (sourceChanged(currentMtimeMs))
    emit sourceConflictDetected(currentMtimeMs);
}

void EditSession::appendAudit(const QString &operation, const QString &detail)
{
  const QString line = QStringLiteral("%1 | %2 | %3")
                           .arg(QDateTime::currentDateTime().toString(Qt::ISODate), operation, detail);
  m_audit << line;
}

QString EditSession::auditSummary() const
{
  return QStringLiteral("%1 条编辑操作\n%2")
      .arg(m_audit.size())
      .arg(m_audit.join(QLatin1Char('\n')));
}

bool EditSession::editable() const
{
  return !m_readOnly;
}

void EditSession::pushCmd(const QString &text, std::function<void()> undoFn,
                          std::function<void()> redoFn, const QString &auditOp,
                          const QString &auditDetail)
{
  auto *stack = m_stack.get();
  stack->push(std::make_unique<EditCommand>(text, std::move(undoFn), std::move(redoFn)));
  appendAudit(auditOp, auditDetail);
}

// ----------------------------------------------------------------------------
// 标志层编辑（D3.1/D3.2）
// ----------------------------------------------------------------------------
static int findMarker(ComprehensiveWellData &doc, const QString &name)
{
  for (int i = 0; i < doc.standardHorizons.size(); ++i)
    if (doc.standardHorizons.at(i).second == name)
      return i;
  return -1;
}

bool EditSession::moveMarker(const QString &name, double newDepth)
{
  if (!editable())
    return false;
  const int idx = findMarker(m_doc, name);
  if (idx < 0)
    return false;

  const double oldDepth = m_doc.standardHorizons.at(idx).first;
  if (std::abs(oldDepth - newDepth) < 1e-6)
    return true;

  pushCmd(tr("移动标志层 %1").arg(name),
          [this, name, oldDepth]() {
            const int i = findMarker(m_doc, name);
            if (i >= 0) m_doc.standardHorizons[i].first = oldDepth;
          },
          [this, name, newDepth]() {
            const int i = findMarker(m_doc, name);
            if (i >= 0) m_doc.standardHorizons[i].first = newDepth;
          },
          QStringLiteral("marker.move"),
          QStringLiteral("%1: %2 -> %3").arg(name, QString::number(oldDepth, 'f', 1),
                                             QString::number(newDepth, 'f', 1)));
  return true;
}

bool EditSession::renameMarker(const QString &oldName, const QString &newName)
{
  if (!editable() || newName.trimmed().isEmpty())
    return false;
  const int idx = findMarker(m_doc, oldName);
  if (idx < 0 || findMarker(m_doc, newName) >= 0)
    return false;

  pushCmd(tr("重命名标志层 %1→%2").arg(oldName, newName),
          [this, oldName, newName]() {
            const int i = findMarker(m_doc, newName);
            if (i >= 0) m_doc.standardHorizons[i].second = oldName;
          },
          [this, oldName, newName]() {
            const int i = findMarker(m_doc, oldName);
            if (i >= 0) m_doc.standardHorizons[i].second = newName;
          },
          QStringLiteral("marker.rename"),
          QStringLiteral("%1 -> %2").arg(oldName, newName));
  return true;
}

bool EditSession::insertMarker(const QString &name, double depth)
{
  if (!editable() || name.trimmed().isEmpty())
    return false;
  if (findMarker(m_doc, name) >= 0)
    return false;

  pushCmd(tr("插入标志层 %1@%2m").arg(name, QString::number(depth, 'f', 1)),
          [this, name]() {
            const int i = findMarker(m_doc, name);
            if (i >= 0) m_doc.standardHorizons.removeAt(i);
          },
          [this, name, depth]() {
            if (findMarker(m_doc, name) < 0)
              m_doc.standardHorizons.append({depth, name});
          },
          QStringLiteral("marker.insert"),
          QStringLiteral("%1 @ %2").arg(name, QString::number(depth, 'f', 1)));
  return true;
}

bool EditSession::removeMarker(const QString &name)
{
  if (!editable())
    return false;
  const int idx = findMarker(m_doc, name);
  if (idx < 0)
    return false;

  const double depth = m_doc.standardHorizons.at(idx).first;
  pushCmd(tr("删除标志层 %1").arg(name),
          [this, name, depth]() {
            if (findMarker(m_doc, name) < 0)
              m_doc.standardHorizons.append({depth, name});
          },
          [this, name]() {
            const int i = findMarker(m_doc, name);
            if (i >= 0) m_doc.standardHorizons.removeAt(i);
          },
          QStringLiteral("marker.delete"),
          QStringLiteral("%1 @ %2").arg(name, QString::number(depth, 'f', 1)));
  return true;
}

// ----------------------------------------------------------------------------
// 岩性区间编辑（D3.4/D3.13）
// ----------------------------------------------------------------------------
bool EditSession::editLithoInterval(int index, const LithologyInterval &interval)
{
  if (!editable() || index < 0 || index >= m_doc.lithologyIntervals.size())
    return false;
  if (interval.bottomDepth <= interval.topDepth)
    return false;

  const LithologyInterval old = m_doc.lithologyIntervals.at(index);
  pushCmd(tr("编辑岩性区间 #%1").arg(index + 1),
          [this, index, old]() { m_doc.lithologyIntervals[index] = old; },
          [this, index, interval]() { m_doc.lithologyIntervals[index] = interval; },
          QStringLiteral("litho.edit"),
          QStringLiteral("#%1: %2 [%3~%4] -> %5 [%6~%7]")
              .arg(QString::number(index + 1), old.lithoName,
                   QString::number(old.topDepth, 'f', 1), QString::number(old.bottomDepth, 'f', 1),
                   interval.lithoName, QString::number(interval.topDepth, 'f', 1),
                   QString::number(interval.bottomDepth, 'f', 1)));
  return true;
}

bool EditSession::removeLithoInterval(int index)
{
  if (!editable() || index < 0 || index >= m_doc.lithologyIntervals.size())
    return false;

  const LithologyInterval old = m_doc.lithologyIntervals.at(index);
  pushCmd(tr("删除岩性区间 #%1").arg(index + 1),
          [this, index, old]() {
            if (index <= m_doc.lithologyIntervals.size())
              m_doc.lithologyIntervals.insert(index, old);
          },
          [this, index]() {
            if (index >= 0 && index < m_doc.lithologyIntervals.size())
              m_doc.lithologyIntervals.removeAt(index);
          },
          QStringLiteral("litho.delete"),
          QStringLiteral("#%1: %2 [%3~%4]")
              .arg(QString::number(index + 1), old.lithoName,
                   QString::number(old.topDepth, 'f', 1), QString::number(old.bottomDepth, 'f', 1)));
  return true;
}

bool EditSession::appendLithoInterval(const LithologyInterval &interval)
{
  if (!editable() || interval.bottomDepth <= interval.topDepth)
    return false;

  const int index = m_doc.lithologyIntervals.size();
  pushCmd(tr("追加岩性区间 %1").arg(interval.lithoName),
          [this, index]() {
            if (index >= 0 && index < m_doc.lithologyIntervals.size())
              m_doc.lithologyIntervals.removeAt(index);
          },
          [this, interval, index]() {
            if (m_doc.lithologyIntervals.size() == index)
              m_doc.lithologyIntervals.append(interval);
          },
          QStringLiteral("litho.append"),
          QStringLiteral("%1 [%2~%3]")
              .arg(interval.lithoName, QString::number(interval.topDepth, 'f', 1),
                   QString::number(interval.bottomDepth, 'f', 1)));
  return true;
}

// ----------------------------------------------------------------------------
// 相区间编辑（D3.5；三级联动校验 intervaleditor 提供，会话只做数据写）
// ----------------------------------------------------------------------------
bool EditSession::editFaciesInterval(int index, const FaciesInterval &interval)
{
  if (!editable() || index < 0 || index >= m_doc.faciesIntervals.size())
    return false;
  if (interval.bottomDepth <= interval.topDepth)
    return false;

  const FaciesInterval old = m_doc.faciesIntervals.at(index);
  pushCmd(tr("编辑相区间 #%1").arg(index + 1),
          [this, index, old]() { m_doc.faciesIntervals[index] = old; },
          [this, index, interval]() { m_doc.faciesIntervals[index] = interval; },
          QStringLiteral("facies.edit"),
          QStringLiteral("#%1: 微相 %2 -> %3")
              .arg(QString::number(index + 1), old.microFacies, interval.microFacies));
  return true;
}

// ----------------------------------------------------------------------------
// 地层指派（D3.6）
// ----------------------------------------------------------------------------
bool EditSession::applyStratAssignments(const QList<StratAssignment> &assignments)
{
  if (!editable())
    return false;

  // 指派不污染源数据：仅影响 stratigraphyIntervals 派生展示列（系/统/组）。
  const auto before = m_doc.stratigraphyIntervals;
  pushCmd(tr("应用地层指派 ×%1").arg(assignments.size()),
          [this, before]() { m_doc.stratigraphyIntervals = before; },
          [this, assignments]() {
            for (auto &si : m_doc.stratigraphyIntervals)
            {
              for (const auto &a : assignments)
              {
                if (si.formation == a.layerName)
                {
                  if (!a.system.isEmpty()) si.system = a.system;
                  if (!a.series.isEmpty()) si.series = a.series;
                }
              }
            }
          },
          QStringLiteral("strat.assign"),
          QStringLiteral("%1 项指派").arg(assignments.size()));
  return true;
}

// ----------------------------------------------------------------------------
// 合并/拆分（D3.13）
// ----------------------------------------------------------------------------
int EditSession::mergeAdjacentLithoIntervals()
{
  if (!editable())
    return 0;

  const auto before = m_doc.lithologyIntervals;
  // 排序后合并相邻同名区间（底=下一段顶）
  auto sorted = before;
  std::sort(sorted.begin(), sorted.end(),
            [](const LithologyInterval &a, const LithologyInterval &b) {
              return a.topDepth < b.topDepth;
            });
  QVector<LithologyInterval> merged;
  int mergePairs = 0;
  for (const auto &li : sorted)
  {
    if (!merged.isEmpty() && merged.last().lithoName == li.lithoName &&
        std::abs(merged.last().bottomDepth - li.topDepth) < 1e-3)
    {
      merged.last().bottomDepth = std::max(merged.last().bottomDepth, li.bottomDepth);
      ++mergePairs;
    }
    else
    {
      merged << li;
    }
  }
  if (mergePairs == 0)
    return 0;

  pushCmd(tr("合并相邻同岩性区间 ×%1").arg(mergePairs),
          [this, before]() { m_doc.lithologyIntervals = before; },
          [this, merged]() { m_doc.lithologyIntervals = merged; },
          QStringLiteral("litho.merge"),
          QStringLiteral("%1 对").arg(mergePairs));
  return mergePairs;
}

bool EditSession::splitLithoInterval(int index, double atDepth)
{
  if (!editable() || index < 0 || index >= m_doc.lithologyIntervals.size())
    return false;
  const auto &src = m_doc.lithologyIntervals.at(index);
  if (atDepth <= src.topDepth + 1e-3 || atDepth >= src.bottomDepth - 1e-3)
    return false;

  const auto before = m_doc.lithologyIntervals;
  pushCmd(tr("拆分岩性区间 #%1@%2m").arg(QString::number(index + 1),
                                         QString::number(atDepth, 'f', 1)),
          [this, before]() { m_doc.lithologyIntervals = before; },
          [this, index, src, atDepth]() {
            if (index >= 0 && index < m_doc.lithologyIntervals.size())
            {
              LithologyInterval upper = src;
              upper.bottomDepth = static_cast<float>(atDepth);
              LithologyInterval lower = src;
              lower.topDepth = static_cast<float>(atDepth);
              m_doc.lithologyIntervals[index] = upper;
              m_doc.lithologyIntervals.insert(index + 1, lower);
            }
          },
          QStringLiteral("litho.split"),
          QStringLiteral("#%1 @ %2").arg(QString::number(index + 1),
                                         QString::number(atDepth, 'f', 1)));
  return true;
}

// ----------------------------------------------------------------------------
// 批量导入（D3.7）
// ----------------------------------------------------------------------------
EditSession::BatchApplyResult EditSession::applyBatchMarkers(
    const QVector<QPair<QString, double>> &rows)
{
  BatchApplyResult res;
  if (!editable())
    return res;

  const auto before = m_doc.standardHorizons;
  auto after = before;

  for (const auto &row : rows)
  {
    const QString &name = row.first;
    const double depth = row.second;
    if (name.trimmed().isEmpty() || depth <= 0.0)
    {
      res.rejected << QStringLiteral("%1,%2").arg(name, QString::number(depth, 'f', 1));
      continue;
    }
    int idx = -1;
    for (int i = 0; i < after.size(); ++i)
      if (after.at(i).second == name)
      {
        idx = i;
        break;
      }
    if (idx < 0)
    {
      after.append({depth, name});
      ++res.inserted;
    }
    else
    {
      // 同名既有：深度不同视为移动（冲突报告已在预校验给出，应用即覆盖）
      if (std::abs(after.at(idx).first - depth) > 1e-6)
      {
        after[idx].first = depth;
        ++res.moved;
      }
      else
      {
        ++res.renamed; // 同名同深 = 幂等跳过（计入 renamed 以示非新增）
      }
    }
  }

  if (res.inserted == 0 && res.moved == 0)
    return res;

  pushCmd(tr("批量导入标志层 +%1 移%2").arg(res.inserted).arg(res.moved),
          [this, before]() { m_doc.standardHorizons = before; },
          [this, after]() { m_doc.standardHorizons = after; },
          QStringLiteral("marker.batch"),
          QStringLiteral("insert=%1 move=%2 reject=%3")
              .arg(res.inserted)
              .arg(res.moved)
              .arg(res.rejected.size()));
  return res;
}

// ----------------------------------------------------------------------------
// 派生文档（D3.3/D3.11）
// ----------------------------------------------------------------------------
ComprehensiveWellData EditSession::buildDerivedDocument() const
{
  ComprehensiveWellData derived = m_doc;
  // 派生版本元数据约定：井名加 DERIVED 后缀标识（壳落 catalog 版本链时使用
  // 版本机制标识；文档内以文本陈述来源）
  derived.wellName = m_doc.wellName;
  // 审计摘要作为文本道区间挂在文档尾部（SpreadsheetML 审计工作表等价物；
  // io 写函数会把 auditLines 序列化为独立「编辑审计」工作表）
  return derived;
}

QString EditSession::buildManifestStyleSummary() const
{
  return QStringLiteral(
             "well=%1\nsource=%2\noperations=%3\n---\n%4")
      .arg(m_doc.wellName,
           m_sourceId.isEmpty() ? QStringLiteral("unknown") : m_sourceId)
      .arg(m_audit.size())
      .arg(m_audit.join(QLatin1Char('\n')));
}

} // namespace WellComposite
