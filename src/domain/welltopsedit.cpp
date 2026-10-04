// 层：数据
#include "welltopsedit.h"

#include <QHash>

#include <cmath>

namespace WellTopsEdit
{
namespace
{
// 层名键：去首尾空白 + 忽略大小写（DC.dat 层名惯例全大写，但统一时
// 「c3」与「C3」应视为同名）。
QString topKey(const QString &name)
{
  return name.trimmed().toUpper();
}

// 井名键：去空白/连字符/下划线 + 忽略大小写（语义对齐
// DataCatalog::normalizeWellName——domain 不链 catalog，本地镜像，
// 两边规则改动需同步）。
QString wellKey(const QString &name)
{
  QString out;
  out.reserve(name.size());
  for (const QChar &c : name)
  {
    if (c.isSpace() || c == QLatin1Char('-') || c == QLatin1Char('_'))
      continue;
    out.append(c.toLower());
  }
  return out;
}


QString rowKey(const WellTopRecord &r)
{
  // QHash<std::pair> 无内建 qHash——单串键（\x1f 作井名/层名分隔符，
  // 规范化键里不可能出现控制字符）。
  return wellKey(r.wellName) + QLatin1Char('\x1f') + topKey(r.topName);
}

const double kEps = 1e-6;

QString depthText(const WellTopRecord &r)
{
  return r.hasMd ? QString::number(r.md, 'f', 3) : QStringLiteral("空");
}
} // namespace

bool Issue::isError() const
{
  switch (kind)
  {
    case IssueKind::DanglingName:
    case IssueKind::MissingTop:
      return false;
    default:
      return true;
  }
}

// 数值写侧/显示共用的自适应精度：3..9 位小数里最短的往返精确表示，
// 都不精确退化 'g' 17。负零归一为正零。
QString formatDepth(double v)
{
  if (v == 0.0)
    v = 0.0; // -0.0 → 0.0（== 比较为真，赋值消除符号位差异）
  for (int n = 3; n <= 9; ++n)
  {
    const QString s = QString::number(v, 'f', n);
    bool ok = false;
    const double back = s.toDouble(&ok);
    if (ok && back == v)
      return s;
  }
  return QString::number(v, 'g', 17);
}

QString issueKindLabel(IssueKind kind)
{
  switch (kind)
  {
    case IssueKind::EmptyName:
      return QStringLiteral("层名为空");
    case IssueKind::WhitespaceName:
      return QStringLiteral("层名含空白");
    case IssueKind::EmptyWellName:
      return QStringLiteral("井名为空");
    case IssueKind::SentinelValue:
      return QStringLiteral("命中缺失哨兵值");
    case IssueKind::DuplicateName:
      return QStringLiteral("层名重复（叠置）");
    case IssueKind::SameDepth:
      return QStringLiteral("同深叠置");
    case IssueKind::TvdOverMd:
      return QStringLiteral("TVD 超过 MD");
    case IssueKind::MdOutOfRange:
      return QStringLiteral("MD 越界");
    case IssueKind::Inversion:
      return QStringLiteral("层序倒置");
    case IssueKind::DanglingName:
      return QStringLiteral("悬空层名");
    case IssueKind::MissingTop:
      return QStringLiteral("层位缺失（空洞）");
  }
  return QString();
}

QVector<Issue> validate(const QVector<WellTopRecord> &rows, const ValidationContext &ctx)
{
  QVector<Issue> issues;

  // 1) 行内基础项：层名/深度域。
  for (int i = 0; i < rows.size(); ++i)
  {
    const WellTopRecord &r = rows.at(i);
    if (r.topName.trimmed().isEmpty())
      issues.append({IssueKind::EmptyName, i,
                     QStringLiteral("第 %1 行：层名为空").arg(i + 1)});
    else if (std::any_of(r.topName.cbegin(), r.topName.cend(),
                         [](QChar c) { return c.isSpace(); }))
      issues.append({IssueKind::WhitespaceName, i,
                     QStringLiteral("第 %1 行：层名「%2」含空白——DC.dat 按空白分列，"
                                    "含空白的层名写盘即损坏")
                         .arg(QString::number(i + 1), r.topName)});
    if (r.wellName.trimmed().isEmpty())
      issues.append({IssueKind::EmptyWellName, i,
                     QStringLiteral("第 %1 行：井名为空——写盘后该行无法解析归属").arg(
                         QString::number(i + 1))});
    if (r.hasMd)
    {
      if (r.md < 0)
        issues.append({IssueKind::MdOutOfRange, i,
                       QStringLiteral("第 %1 行（%2）：MD 为负值 %3")
                           .arg(QString::number(i + 1), r.topName,
                                QString::number(r.md, 'f', 3))});
      else if (ctx.hasTd && r.md > ctx.td + kEps)
        issues.append({IssueKind::MdOutOfRange, i,
                       QStringLiteral("第 %1 行（%2）：MD %3 超过井 TD %4")
                           .arg(QString::number(i + 1), r.topName,
                                QString::number(r.md, 'f', 3),
                                QString::number(ctx.td, 'f', 3))});
    }
    if (r.hasMd && r.hasTvd && r.tvd > r.md + kEps)
      issues.append({IssueKind::TvdOverMd, i,
                     QStringLiteral("第 %1 行（%2）：TVD %3 大于 MD %4")
                         .arg(QString::number(i + 1), r.topName,
                              QString::number(r.tvd, 'f', 3),
                              QString::number(r.md, 'f', 3))});
    {
      const struct
      {
        bool has;
        double v;
        const char *col;
      } sentinelCols[] = {{r.hasTvd, r.tvd, "TVD"}, {r.hasX, r.x, "X"},
                          {r.hasY, r.y, "Y"},        {r.hasTime, r.timeMs, "Time(ms)"}};
      for (const auto &c : sentinelCols)
        if (c.has && c.v <= -99998.5)
          issues.append({IssueKind::SentinelValue, i,
                         QStringLiteral("第 %1 行（%2）：%3 列值 %4 命中缺失哨兵域"
                                        "（≤ -99999）——缺失请清空单元格")
                             .arg(QString::number(i + 1), r.topName,
                                  QString::fromLatin1(c.col),
                                  QString::number(c.v, 'f', 3))});
    }
  }

  // 2) 同井叠置：层名重复 / 同深。
  {
    QHash<QString, QVector<int>> byName;
    QHash<qint64, QVector<int>> byDepth; // MD 按 1e-6 量化
    for (int i = 0; i < rows.size(); ++i)
    {
      const WellTopRecord &r = rows.at(i);
      byName[topKey(r.topName)].append(i);
      if (r.hasMd)
        byDepth[qRound64(r.md / kEps)].append(i);
    }
    for (auto it = byName.constBegin(); it != byName.constEnd(); ++it)
    {
      if (it.value().size() <= 1)
        continue;
      for (int i : it.value())
        issues.append({IssueKind::DuplicateName, i,
                       QStringLiteral("第 %1 行（%2）：层名与第 %3 行重复（共 %4 行）")
                           .arg(QString::number(i + 1), rows.at(i).topName,
                                QString::number(it.value().front() + 1),
                                QString::number(it.value().size()))});
    }
    for (auto it = byDepth.constBegin(); it != byDepth.constEnd(); ++it)
    {
      if (it.value().size() <= 1)
        continue;
      for (int i : it.value())
        issues.append({IssueKind::SameDepth, i,
                       QStringLiteral("第 %1 行（%2）：MD %3 与第 %4 行相同")
                           .arg(QString::number(i + 1), rows.at(i).topName,
                                depthText(rows.at(i)),
                                QString::number(it.value().front() + 1))});
    }
  }

  // 3) 格架联动（无词表即跳过——诚实降级，不伪造基准）。
  if (!ctx.framework.isEmpty())
  {
    QHash<QString, int> order;
    for (int k = 0; k < ctx.framework.size(); ++k)
      order.insert(topKey(ctx.framework.at(k)), k);

    // 3a) 倒置：格架序（浅→深）与 MD 深度序冲突——逐对检出，两行各一条。
    QVector<QPair<int, int>> known; // (行号, 格架序)
    for (int i = 0; i < rows.size(); ++i)
    {
      const auto it = order.constFind(topKey(rows.at(i).topName));
      if (it != order.constEnd() && rows.at(i).hasMd)
        known.append({i, it.value()});
    }
    for (int a = 0; a < known.size(); ++a)
      for (int b = a + 1; b < known.size(); ++b)
      {
        const int i = known.at(a).first, j = known.at(b).first;
        const int ki = known.at(a).second, kj = known.at(b).second;
        if (ki == kj)
          continue; // 同层名多行的深度互斥归 DuplicateName，不重复报倒置
        const bool iShallower = ki < kj;
        const double mdI = rows.at(i).md, mdJ = rows.at(j).md;
        const bool inverted = iShallower ? (mdI > mdJ + kEps) : (mdJ > mdI + kEps);
        if (!inverted)
          continue;
        const QString msg = QStringLiteral("第 %1 行（%2，MD %3）与第 %4 行（%5，MD %6）："
                                           "层序 %7 浅于 %8 但深度更大")
                                .arg(QString::number(i + 1), rows.at(i).topName,
                                     QString::number(mdI, 'f', 3),
                                     QString::number(j + 1), rows.at(j).topName,
                                     QString::number(mdJ, 'f', 3),
                                     ctx.framework.at(ki), ctx.framework.at(kj));
        issues.append({IssueKind::Inversion, i, msg});
        issues.append({IssueKind::Inversion, j, msg});
      }

    // 3b) 悬空层名：名单外层位（警告；空名归 EmptyName，不重复计）。
    for (int i = 0; i < rows.size(); ++i)
      if (!rows.at(i).topName.trimmed().isEmpty() && !order.contains(topKey(rows.at(i).topName)))
        issues.append({IssueKind::DanglingName, i,
                       QStringLiteral("第 %1 行：层名「%2」不在层序格架词表内")
                           .arg(QString::number(i + 1), rows.at(i).topName)});

    // 3c) 空洞：格架层位缺失（警告）。锚 = 按层序最近的既有浅侧行。
    QHash<int, bool> present;
    for (int i = 0; i < rows.size(); ++i)
      present.insert(order.value(topKey(rows.at(i).topName), -1), true);
    for (int k = 0; k < ctx.framework.size(); ++k)
    {
      if (present.contains(k))
        continue;
      int anchor = -1, anchorOrder = -1;
      for (int i = 0; i < rows.size(); ++i)
      {
        const int ko = order.value(topKey(rows.at(i).topName), -1);
        if (ko < 0 || ko >= k) // 只找严格浅于 k 的既有行
          continue;
        if (ko > anchorOrder)
        {
          anchorOrder = ko;
          anchor = i;
        }
      }
      const QString below = (k + 1 < ctx.framework.size()) ? ctx.framework.at(k + 1)
                                                           : QStringLiteral("井底");
      issues.append({IssueKind::MissingTop, anchor,
                     QStringLiteral("缺失层位「%1」（按层序应位于 %2 与 %3 之间）")
                         .arg(ctx.framework.at(k),
                              anchor >= 0 ? rows.at(anchor).topName
                                          : QStringLiteral("最浅层之上"),
                              below)});
    }
  }

  return issues;
}

bool sameTop(const WellTopRecord &a, const WellTopRecord &b)
{
  return topKey(a.topName) == topKey(b.topName) && a.hasMd == b.hasMd && a.hasX == b.hasX &&
         a.hasY == b.hasY && a.hasTvd == b.hasTvd && a.hasTime == b.hasTime &&
         (!a.hasMd || std::abs(a.md - b.md) <= kEps) &&
         (!a.hasX || std::abs(a.x - b.x) <= kEps) &&
         (!a.hasY || std::abs(a.y - b.y) <= kEps) &&
         (!a.hasTvd || std::abs(a.tvd - b.tvd) <= kEps) &&
         (!a.hasTime || std::abs(a.timeMs - b.timeMs) <= kEps) &&
         std::abs(a.z - b.z) <= kEps;
}

bool sameValues(const WellTopRecord &a, const WellTopRecord &b)
{
  return wellKey(a.wellName) == wellKey(b.wellName) && sameTop(a, b);
}

DiffSummary diff(const QVector<WellTopRecord> &oldRows, const QVector<WellTopRecord> &newRows)
{
  QHash<QString, const WellTopRecord *> oldIdx;
  for (const WellTopRecord &r : oldRows)
    if (!oldIdx.contains(rowKey(r))) // 键重复（数据异常）首见为准
      oldIdx.insert(rowKey(r), &r);
  QHash<QString, const WellTopRecord *> newIdx;
  for (const WellTopRecord &r : newRows)
    if (!newIdx.contains(rowKey(r)))
      newIdx.insert(rowKey(r), &r);

  DiffSummary s;
  for (auto it = newIdx.constBegin(); it != newIdx.constEnd(); ++it)
  {
    const auto oldIt = oldIdx.constFind(it.key());
    if (oldIt == oldIdx.constEnd())
      ++s.added;
    else if (!sameValues(*oldIt.value(), *it.value()))
      ++s.changed;
  }
  for (auto it = oldIdx.constBegin(); it != oldIdx.constEnd(); ++it)
    if (!newIdx.contains(it.key()))
      ++s.removed;
  return s;
}

int applyRename(QVector<WellTopRecord> *rows, const QString &from, const QString &to)
{
  // from/to 键相同也放行——「c3 → C3」拼写统一是合法重命名（只改显示形）。
  const QString key = topKey(from);
  if (key.isEmpty() || to.trimmed().isEmpty())
    return 0;
  int changed = 0;
  for (WellTopRecord &r : *rows)
    if (topKey(r.topName) == key)
    {
      r.topName = to;
      ++changed;
    }
  return changed;
}

int applyShift(QVector<WellTopRecord> *rows, double delta)
{
  if (std::abs(delta) <= kEps)
    return 0;
  int changed = 0;
  for (WellTopRecord &r : *rows)
  {
    const bool touches = r.hasMd || r.hasTvd || (r.hasX && r.hasY);
    if (!touches)
      continue;
    if (r.hasMd)
      r.md += delta;
    if (r.hasTvd)
      r.tvd += delta;
    if (r.hasX && r.hasY) // Z 与 X/Y 同列组（解析器 t.size()>=6 才读）
      r.z += delta;
    ++changed;
  }
  return changed;
}

int applyDelete(QVector<WellTopRecord> *rows, const QString &topName)
{
  const QString key = topKey(topName);
  if (key.isEmpty())
    return 0;
  int removed = 0;
  for (int i = 0; i < rows->size();)
  {
    if (topKey(rows->at(i).topName) == key)
    {
      rows->removeAt(i);
      ++removed;
    }
    else
      ++i;
  }
  return removed;
}

bool MergeRow::conflicts() const
{
  return inOld && inNew && !sameValues(oldRec, newRec);
}

QVector<MergeRow> mergeDiff(const QVector<WellTopRecord> &oldRows,
                            const QVector<WellTopRecord> &incoming)
{
  QVector<MergeRow> out;
  QHash<QString, int> byKey; // topKey → out 行号（同键重复以首见为准）
  for (const WellTopRecord &r : oldRows)
  {
    if (byKey.contains(topKey(r.topName))) // 键重复（数据异常）首见为准
      continue;
    MergeRow m;
    m.topName = r.topName;
    m.inOld = true;
    m.oldRec = r;
    byKey.insert(topKey(r.topName), out.size());
    out.append(m);
  }
  for (const WellTopRecord &r : incoming)
  {
    const QString key = topKey(r.topName);
    const auto it = byKey.constFind(key);
    if (it != byKey.constEnd())
    {
      MergeRow &m = out[it.value()];
      if (!m.inNew) // 同键重复（数据异常）首见为准
      {
        m.inNew = true;
        m.newRec = r;
      }
      continue;
    }
    MergeRow m;
    m.topName = r.topName;
    m.inNew = true;
    m.newRec = r;
    m.resolution = MergeRow::Resolution::TakeNew; // 仅新行默认新增
    byKey.insert(key, out.size());
    out.append(m);
  }
  return out;
}

QVector<WellTopRecord> applyMerge(const QVector<MergeRow> &rows)
{
  QVector<WellTopRecord> out;
  out.reserve(rows.size());
  for (const MergeRow &m : rows)
  {
    switch (m.resolution)
    {
      case MergeRow::Resolution::KeepOld:
        if (m.inOld)
          out.append(m.oldRec);
        break;
      case MergeRow::Resolution::TakeNew:
        if (m.inNew)
          out.append(m.newRec);
        break;
      case MergeRow::Resolution::RemoveOld:
        break;
    }
  }
  return out;
}

} // namespace WellTopsEdit
