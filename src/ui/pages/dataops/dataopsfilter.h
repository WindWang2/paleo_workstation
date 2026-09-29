// 层：视图
// ui/pages/dataops/dataopsfilter — D2 搜索/过滤/组织：多维条件、AND/OR 组合、
// chip、保存的过滤器、URL-like 状态串。纯逻辑（无 QWidget），匹配输入是
// dataopsmodel.h 的 AssetRowInfo 快照（过滤循环零 catalog 回查——D2.8）。
#pragma once

#include <QRegularExpression>
#include <QSettings>
#include <QSharedData>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <QVector>

#include "dataopsmodel.h"

namespace paleo::dataops
{

// ---- 过滤维度 --------------------------------------------------------------
enum class FilterDim
{
  Search,    // 自由文本（名称/类型/关联 contains）
  Type,      // 资产类型（effectiveType 口径——含用户改写）
  Status,    // RAW / DERIVED
  Role,      // 任一链接角色
  Entity,    // 归属实体名
  Tag,       // 用户标签
  Regex,     // 文件名正则（QRegularExpression，无效模式按字面量处理）
  Unlinked,  // 未挂接（无已决实体链接）
  UnknownType, // 类型 unknown/空
  Warned,    // 警告状态（未决链接存在）
};

inline QString filterDimKey(FilterDim d)
{
  switch (d)
  {
    case FilterDim::Search: return QStringLiteral("q");
    case FilterDim::Type: return QStringLiteral("type");
    case FilterDim::Status: return QStringLiteral("status");
    case FilterDim::Role: return QStringLiteral("role");
    case FilterDim::Entity: return QStringLiteral("entity");
    case FilterDim::Tag: return QStringLiteral("tag");
    case FilterDim::Regex: return QStringLiteral("re");
    case FilterDim::Unlinked: return QStringLiteral("unlinked");
    case FilterDim::UnknownType: return QStringLiteral("unknownType");
    case FilterDim::Warned: return QStringLiteral("warned");
  }
  return QString();
}

inline QString filterDimLabel(FilterDim d)
{
  switch (d)
  {
    case FilterDim::Search: return QStringLiteral("搜索");
    case FilterDim::Type: return QStringLiteral("类型");
    case FilterDim::Status: return QStringLiteral("状态");
    case FilterDim::Role: return QStringLiteral("角色");
    case FilterDim::Entity: return QStringLiteral("实体");
    case FilterDim::Tag: return QStringLiteral("标签");
    case FilterDim::Regex: return QStringLiteral("正则");
    case FilterDim::Unlinked: return QStringLiteral("未挂接");
    case FilterDim::UnknownType: return QStringLiteral("类型未知");
    case FilterDim::Warned: return QStringLiteral("有警告");
  }
  return QString();
}

// ---- 单条件 ----------------------------------------------------------------
struct FilterCondition
{
  FilterDim dim = FilterDim::Search;
  QString value;
  bool negate = false; // D2.2 chip 单删 + 取反（「非 DERIVED」）

  bool operator==(const FilterCondition &o) const
  {
    return dim == o.dim && value == o.value && negate == o.negate;
  }

  // chip 文案：「类型: well_log」「非 状态: DERIVED」。
  QString display() const
  {
    const QString base = filterDimLabel(dim) + QStringLiteral(": ") + value;
    return negate ? QStringLiteral("非 ") + base : base;
  }

  // 单条件匹配（negate 在此应用）。
  bool matches(const AssetRowInfo &row) const
  {
    bool hit = false;
    switch (dim)
    {
      case FilterDim::Search:
      {
        const QString needle = value.trimmed();
        hit = needle.isEmpty() ||
              row.displayName.contains(needle, Qt::CaseInsensitive) ||
              row.effectiveType.contains(needle, Qt::CaseInsensitive) ||
              row.entityNames.join(QLatin1Char(',')).contains(needle, Qt::CaseInsensitive) ||
              row.fileName.contains(needle, Qt::CaseInsensitive);
        break;
      }
      case FilterDim::Type:
        hit = row.effectiveType.compare(value, Qt::CaseInsensitive) == 0;
        break;
      case FilterDim::Status:
        hit = row.status.compare(value, Qt::CaseInsensitive) == 0;
        break;
      case FilterDim::Role:
        hit = row.roles.contains(value, Qt::CaseInsensitive);
        break;
      case FilterDim::Entity:
        hit = row.entityNames.contains(value, Qt::CaseInsensitive);
        break;
      case FilterDim::Tag:
        hit = row.tags.contains(value, Qt::CaseInsensitive);
        break;
      case FilterDim::Regex:
      {
        if (value.isEmpty())
        {
          hit = true;
          break;
        }
        QRegularExpression re(value);
        if (!re.isValid())
          re = QRegularExpression(QRegularExpression::escape(value)); // 无效模式退字面量
        hit = re.match(row.fileName).hasMatch() || re.match(row.displayName).hasMatch();
        break;
      }
      case FilterDim::Unlinked:
        hit = row.entityNames.isEmpty();
        break;
      case FilterDim::UnknownType:
        hit = row.effectiveType.isEmpty() ||
              row.effectiveType.compare(QStringLiteral("unknown"), Qt::CaseInsensitive) == 0;
        break;
      case FilterDim::Warned:
        hit = row.unresolved;
        break;
    }
    return negate ? !hit : hit;
  }
};

// ---- 条件组（AND / OR，D2.2）----------------------------------------------
struct FilterGroup
{
  QVector<FilterCondition> conditions;
  bool orMode = false; // false = AND（缺省），true = OR

  bool isEmpty() const { return conditions.isEmpty(); }
  void add(const FilterCondition &c) { conditions.append(c); }
  // D2.2 chip 单删：按身份删一条（返回是否删到）。
  bool removeOne(const FilterCondition &c)
  {
    for (int i = 0; i < conditions.size(); ++i)
      if (conditions.at(i) == c)
      {
        conditions.removeAt(i);
        return true;
      }
    return false;
  }
  // 同维度取值集合（下拉装填用）。
  QStringList valuesOf(FilterDim d) const
  {
    QStringList out;
    for (const FilterCondition &c : conditions)
      if (c.dim == d)
        out << (c.negate ? QStringLiteral("!") + c.value : c.value);
    return out;
  }
  void clearDim(FilterDim d)
  {
    for (int i = conditions.size() - 1; i >= 0; --i)
      if (conditions.at(i).dim == d)
        conditions.removeAt(i);
  }

  bool matches(const AssetRowInfo &row) const
  {
    if (conditions.isEmpty())
      return true;
    if (orMode)
    {
      for (const FilterCondition &c : conditions)
        if (c.matches(row))
          return true;
      return false;
    }
    for (const FilterCondition &c : conditions)
      if (!c.matches(row))
        return false;
    return true;
  }

  // chip 文案全集（UI 渲染顺序 = 添加顺序）。
  QStringList chipTexts() const
  {
    QStringList out;
    for (const FilterCondition &c : conditions)
      out << c.display();
    return out;
  }

  // ---- D2.10 URL-like 状态串 ----------------------------------------------
  // paleo://dataops-filter?op=or&q=%E6%90%9C&type=well_log&tag=!%E6%A0%B8%E5%BF%83
  // 「!」前缀 = negate。解析失败回空组（可复制共享，坏串不炸）。
  QString toStateString() const
  {
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("op"), orMode ? QStringLiteral("or") : QStringLiteral("and"));
    for (const FilterCondition &c : conditions)
      q.addQueryItem(filterDimKey(c.dim),
                     (c.negate ? QStringLiteral("!") : QString()) + c.value);
    QUrl url;
    url.setScheme(QStringLiteral("paleo"));
    url.setHost(QStringLiteral("dataops-filter"));
    url.setQuery(q);
    return url.toString(QUrl::FullyEncoded);
  }

  static FilterGroup fromStateString(const QString &s)
  {
    FilterGroup g;
    const QUrl url(s);
    if (!url.isValid() || url.host() != QStringLiteral("dataops-filter"))
      return g;
    const QUrlQuery q(url);
    g.orMode = q.queryItemValue(QStringLiteral("op")) == QStringLiteral("or");
    const QList<QPair<QString, QString>> items = q.queryItems();
    for (const auto &kv : items)
    {
      if (kv.first == QStringLiteral("op"))
        continue;
      FilterCondition c;
      bool known = true;
      if (kv.first == QLatin1String("q")) c.dim = FilterDim::Search;
      else if (kv.first == QLatin1String("type")) c.dim = FilterDim::Type;
      else if (kv.first == QLatin1String("status")) c.dim = FilterDim::Status;
      else if (kv.first == QLatin1String("role")) c.dim = FilterDim::Role;
      else if (kv.first == QLatin1String("entity")) c.dim = FilterDim::Entity;
      else if (kv.first == QLatin1String("tag")) c.dim = FilterDim::Tag;
      else if (kv.first == QLatin1String("re")) c.dim = FilterDim::Regex;
      else if (kv.first == QLatin1String("unlinked")) c.dim = FilterDim::Unlinked;
      else if (kv.first == QLatin1String("unknownType")) c.dim = FilterDim::UnknownType;
      else if (kv.first == QLatin1String("warned")) c.dim = FilterDim::Warned;
      else known = false;
      if (!known)
        continue;
      c.value = kv.second;
      if (c.value.startsWith(QLatin1Char('!')))
      {
        c.negate = true;
        c.value = c.value.mid(1);
      }
      g.conditions.append(c);
    }
    return g;
  }
};

// ---- D2.6 保存的过滤器（命名预设）-----------------------------------------
// QSettings paleo/paleo，键 dataops/filterPresets（name → 状态串）。
inline QStringList savedFilterNames()
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  return s.value(QStringLiteral("dataops/filterPresets")).toMap().keys();
}

inline bool saveFilterPreset(const QString &name, const FilterGroup &g)
{
  const QString n = name.trimmed();
  if (n.isEmpty() || n.size() > 40)
    return false;
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  QVariantMap m = s.value(QStringLiteral("dataops/filterPresets")).toMap();
  m.insert(n, g.toStateString());
  s.setValue(QStringLiteral("dataops/filterPresets"), m);
  return true;
}

inline FilterGroup loadFilterPreset(const QString &name)
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  const QVariantMap m = s.value(QStringLiteral("dataops/filterPresets")).toMap();
  return FilterGroup::fromStateString(m.value(name).toString());
}

inline bool deleteFilterPreset(const QString &name)
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  QVariantMap m = s.value(QStringLiteral("dataops/filterPresets")).toMap();
  if (!m.contains(name))
    return false;
  m.remove(name);
  s.setValue(QStringLiteral("dataops/filterPresets"), m);
  return true;
}

// ---- D2.5 树排序记忆 --------------------------------------------------------
enum class TreeSortKind { Name, Time, Type, Size };
inline QString treeSortKey(TreeSortKind k)
{
  switch (k)
  {
    case TreeSortKind::Name: return QStringLiteral("name");
    case TreeSortKind::Time: return QStringLiteral("time");
    case TreeSortKind::Type: return QStringLiteral("type");
    case TreeSortKind::Size: return QStringLiteral("size");
  }
  return QStringLiteral("name");
}
inline void rememberTreeSort(TreeSortKind k)
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  s.setValue(QStringLiteral("dataops/treeSort"), treeSortKey(k));
}
inline TreeSortKind recalledTreeSort()
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  const QString v = s.value(QStringLiteral("dataops/treeSort")).toString();
  if (v == QLatin1String("time")) return TreeSortKind::Time;
  if (v == QLatin1String("type")) return TreeSortKind::Type;
  if (v == QLatin1String("size")) return TreeSortKind::Size;
  return TreeSortKind::Name;
}

// ---- D7.4 稳定多列排序（次级排序键）----------------------------------------
// 主键由用户点列头给出；次级键固定（名称自然序兜底）保证稳定。
struct SortKeys
{
  int column = -1;             // 主键列（-1 = 无排序）
  Qt::SortOrder order = Qt::AscendingOrder;
  QList<QPair<int, Qt::SortOrder>> secondary; // 次级键链

  bool isValid() const { return column >= 0; }
};

// 对行快照按「主键(名称/类型/关联/大小/时间/版本) + 次级链」稳定排序。
// 比较器对 AssetRowInfo；列语义由调用方映射（表列 → 字段）。
inline bool assetRowLess(const AssetRowInfo &a, const AssetRowInfo &b,
                         const QString &field, bool asc)
{
  int cmp = 0;
  if (field == QLatin1String("type"))
    cmp = a.effectiveType.compare(b.effectiveType, Qt::CaseInsensitive);
  else if (field == QLatin1String("time"))
    cmp = a.lastModified < b.lastModified ? -1 : a.lastModified > b.lastModified ? 1 : 0;
  else if (field == QLatin1String("size"))
    cmp = a.sizeBytes < b.sizeBytes ? -1 : a.sizeBytes > b.sizeBytes ? 1 : 0;
  else if (field == QLatin1String("version"))
    cmp = a.currentVersionNo < b.currentVersionNo ? -1 : a.currentVersionNo > b.currentVersionNo ? 1 : 0;
  else if (field == QLatin1String("entity"))
    cmp = a.entityNames.join(QLatin1Char(',')).compare(
        b.entityNames.join(QLatin1Char(',')), Qt::CaseInsensitive);
  else
    cmp = a.displayName.compare(b.displayName, Qt::CaseInsensitive);
  return asc ? cmp < 0 : cmp > 0;
}

inline void sortAssetRows(QVector<AssetRowInfo> *rows, const QString &primaryField,
                          bool asc)
{
  if (!rows)
    return;
  std::stable_sort(rows->begin(), rows->end(), [primaryField, asc](const AssetRowInfo &a,
                                                                  const AssetRowInfo &b) {
    // 次级键：名称（稳定序兜底——同主键值保持名称序）。
    if (assetRowLess(a, b, primaryField, asc))
      return true;
    if (assetRowLess(b, a, primaryField, asc))
      return false;
    return assetRowLess(a, b, QStringLiteral("name"), true);
  });
}

// ---- D2.7 命中区间（高亮委托消费）------------------------------------------
// 返回 text 中所有 needle 的区间 [start, length)（大小写不敏感；needle 空 → 空）。
inline QList<QPair<int, int>> matchRanges(const QString &text, const QString &needle)
{
  QList<QPair<int, int>> out;
  if (needle.isEmpty() || text.isEmpty())
    return out;
  const QString &hay = text;
  const QString &nd = needle;
  int from = 0;
  while (from <= hay.size() - nd.size())
  {
    const int idx = hay.indexOf(nd, from, Qt::CaseInsensitive);
    if (idx < 0)
      break;
    out.append({idx, nd.size()});
    from = idx + nd.size();
  }
  return out;
}

} // namespace paleo::dataops
