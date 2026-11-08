// 层：视图
// ui/pages/dataops/dataopsquery — 见头文件。实现保持纯值：QtCore + 行快照，
// 不碰 catalog/磁盘。
#include "dataopsquery.h"

#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace paleo::dataops::query
{

// ---- 列面 --------------------------------------------------------------------

QString columnKeyToken(ColumnKey k)
{
  switch (k)
  {
    case ColumnKey::Name: return QStringLiteral("name");
    case ColumnKey::Type: return QStringLiteral("type");
    case ColumnKey::Entity: return QStringLiteral("entity");
    case ColumnKey::Tag: return QStringLiteral("tag");
    case ColumnKey::Status: return QStringLiteral("status");
    case ColumnKey::Size: return QStringLiteral("size");
    case ColumnKey::Version: return QStringLiteral("version");
    case ColumnKey::Time: return QStringLiteral("time");
  }
  return QString();
}

QString columnKeyLabel(ColumnKey k)
{
  switch (k)
  {
    case ColumnKey::Name: return QObject::tr("名称");
    case ColumnKey::Type: return QObject::tr("类型");
    case ColumnKey::Entity: return QObject::tr("关联");
    case ColumnKey::Tag: return QObject::tr("标签");
    case ColumnKey::Status: return QObject::tr("状态");
    case ColumnKey::Size: return QObject::tr("大小");
    case ColumnKey::Version: return QObject::tr("版本");
    case ColumnKey::Time: return QObject::tr("时间");
  }
  return QString();
}

bool columnKeyFromToken(const QString &token, ColumnKey *out)
{
  const struct
  {
    const char *t;
    ColumnKey k;
  } kMap[] = {
      {"name", ColumnKey::Name},     {"type", ColumnKey::Type},
      {"entity", ColumnKey::Entity}, {"tag", ColumnKey::Tag},
      {"status", ColumnKey::Status}, {"size", ColumnKey::Size},
      {"version", ColumnKey::Version}, {"time", ColumnKey::Time},
  };
  for (const auto &m : kMap)
    if (token == QLatin1String(m.t))
    {
      if (out)
        *out = m.k;
      return true;
    }
  return false;
}

QStringList columnValuesOf(ColumnKey k, const AssetRowInfo &row)
{
  switch (k)
  {
    case ColumnKey::Name: return {row.displayName};
    case ColumnKey::Type:
      return row.effectiveType.isEmpty() ? QStringList{} : QStringList{row.effectiveType};
    case ColumnKey::Entity: return row.entityNames;
    case ColumnKey::Tag: return row.tags;
    case ColumnKey::Status: return {row.status};
    case ColumnKey::Size:
      return row.sizeBytes > 0 ? QStringList{QString::number(row.sizeBytes)}
                               : QStringList{};
    case ColumnKey::Version:
      return {QStringLiteral("v%1").arg(row.currentVersionNo)};
    case ColumnKey::Time:
      return row.lastModified.isValid()
                 ? QStringList{row.lastModified.toString(QStringLiteral("yyyy-MM-dd"))}
                 : QStringList{};
  }
  return {};
}

bool columnRowHasValue(ColumnKey k, const AssetRowInfo &row, const QString &v)
{
  const bool ci = k == ColumnKey::Name || k == ColumnKey::Type ||
                  k == ColumnKey::Entity || k == ColumnKey::Tag ||
                  k == ColumnKey::Status;
  const QStringList vals = columnValuesOf(k, row);
  if (ci)
    return vals.contains(v, Qt::CaseInsensitive);
  return vals.contains(v, Qt::CaseSensitive);
}

QString ColumnConstraint::display() const
{
  return QObject::tr("列·%1: %2")
      .arg(columnKeyLabel(key),
           values.isEmpty() ? QObject::tr("（空集，无匹配）") : values.join(QStringLiteral(" / ")));
}

// ---- DataQuery 编辑语义 --------------------------------------------------------

bool DataQuery::addCondition(const FilterCondition &c)
{
  if (group.conditions.contains(c))
    return false; // 重复条件规范化：语义等价，不再造同值 chip
  group.add(c);
  return true;
}

bool DataQuery::removeFirstMatching(const FilterCondition &c)
{
  return group.removeOne(c);
}

bool DataQuery::toggleFirstMatching(const FilterCondition &c)
{
  for (FilterCondition &cc : group.conditions)
    if (cc == c)
    {
      cc.negate = !cc.negate;
      return true;
    }
  return false;
}

int firstPositiveIndex(const QVector<FilterCondition> &conds, FilterDim d)
{
  for (int i = 0; i < conds.size(); ++i)
    if (conds.at(i).dim == d && !conds.at(i).negate)
      return i;
  return -1;
}

bool DataQuery::setPositiveValue(FilterDim d, const QString &value)
{
  const int idx = firstPositiveIndex(group.conditions, d);
  if (value.isEmpty())
  {
    if (idx < 0)
      return false; // 控件清空但状态里只有否定条件 → 保留（W1 核心语义）
    group.conditions.removeAt(idx);
    return true;
  }
  if (idx >= 0)
  {
    if (group.conditions[idx].value == value)
      return false;
    group.conditions[idx].value = value;
    return true;
  }
  group.conditions.prepend({d, value, false});
  return true;
}

bool DataQuery::setSwitch(FilterDim d, bool on)
{
  FilterCondition probe;
  probe.dim = d;
  const bool has = group.conditions.contains(probe);
  if (on && !has)
  {
    group.add(probe);
    return true;
  }
  if (!on && has)
    return group.removeOne(probe); // 只删正向；否定同维条件保留
  return false;
}

bool DataQuery::hasPositive(FilterDim d) const
{
  return firstPositiveIndex(group.conditions, d) >= 0;
}

void DataQuery::clearAll()
{
  group.conditions.clear();
  group.orMode = false;
  activeTag.clear();
  columns.clear();
}

const ColumnConstraint *DataQuery::column(ColumnKey k) const
{
  for (const ColumnConstraint &c : columns)
    if (c.key == k)
      return &c;
  return nullptr;
}

void DataQuery::setColumnValues(ColumnKey k, const QStringList &values)
{
  QSet<QString> seen;
  QStringList sorted;
  for (const QString &v : values)
    if (!seen.contains(v))
    {
      seen.insert(v);
      sorted << v;
    }
  sorted.sort();
  for (ColumnConstraint &c : columns)
    if (c.key == k)
    {
      c.values = sorted;
      return;
    }
  ColumnConstraint c;
  c.key = k;
  c.values = sorted;
  columns.append(c);
}

void DataQuery::clearColumn(ColumnKey k)
{
  for (int i = columns.size() - 1; i >= 0; --i)
    if (columns.at(i).key == k)
      columns.removeAt(i);
}

QStringList DataQuery::columnCandidates(ColumnKey k,
                                        const QVector<AssetRowInfo> &rows) const
{
  DataQuery rest = *this;
  rest.clearColumn(k); // 本列不限；组+标签+其他列约束仍生效（复审修正）
  const CompiledQuery compiled = CompiledQuery::compile(rest);
  QSet<QString> found;
  if (const ColumnConstraint *cur = column(k))
    for (const QString &v : cur->values)
      found.insert(v); // 零命中的已选值仍可见、可取消
  for (const AssetRowInfo &row : rows)
    if (compiled.matches(row))
      for (const QString &v : columnValuesOf(k, row))
        found.insert(v);
  QStringList out = found.values();
  out.sort();
  return out;
}

// ---- CompiledQuery --------------------------------------------------------------

CompiledQuery CompiledQuery::compile(const DataQuery &q)
{
  CompiledQuery out;
  out.m_empty = q.isEmpty();
  out.m_orMode = q.group.orMode;
  out.m_tagNeedle = q.activeTag; // 原值比较（旧 applyListFilter 口径）
  for (const FilterCondition &c : q.group.conditions)
  {
    CompiledCondition cc;
    cc.src = c;
    switch (c.dim)
    {
      case FilterDim::Search:
        cc.needle = c.value.trimmed(); // 旧语义：Search trim，空词 = 命中
        break;
      case FilterDim::Type:
      case FilterDim::Status:
      case FilterDim::Role:
      case FilterDim::Entity:
      case FilterDim::Tag:
        break; // 沿用原值 CI 比较（不 trim、空值零命中——逐位对齐旧 matcher）
      case FilterDim::Regex:
        if (!c.value.isEmpty())
        {
          QRegularExpression re(c.value);
          ++out.m_regexCompiles; // 证据面：编译次数 ∝ 条件数，与行数无关
          if (!re.isValid())
          {
            cc.re = QRegularExpression(QRegularExpression::escape(c.value));
            cc.regexLiteral = true; // 既有契约：无效模式按字面量，如实呈现
          }
          else
            cc.re = re;
        }
        break;
      case FilterDim::Unlinked:
      case FilterDim::UnknownType:
      case FilterDim::Warned:
        break; // 开关型：无准备
    }
    out.m_conds.append(cc);
  }
  for (const ColumnConstraint &col : q.columns)
  {
    CompiledColumn cc;
    cc.key = col.key;
    cc.raw = col.values;
    out.m_cols.append(cc);
  }
  // 摘要与匹配共用本次准备（无第二套规则）。
  for (const FilterCondition &c : q.group.conditions)
  {
    ConditionSummary s;
    s.text = c.display();
    s.regexLiteralFallback = false;
    if (c.dim == FilterDim::Regex && !c.value.isEmpty())
    {
      QRegularExpression probe(c.value);
      if (!probe.isValid())
      {
        s.regexLiteralFallback = true;
        s.text = QObject::tr("%1（无效模式——已按字面量「%2」匹配）").arg(s.text, c.value);
      }
    }
    out.m_summaries.append(s);
  }
  if (!q.activeTag.isEmpty())
  {
    ConditionSummary s;
    s.text = QObject::tr("标签: %1").arg(q.activeTag);
    out.m_summaries.append(s);
  }
  for (const ColumnConstraint &col : q.columns)
  {
    ConditionSummary s;
    s.text = col.display();
    out.m_summaries.append(s);
  }
  return out;
}

bool CompiledQuery::matchCondition(const CompiledCondition &c,
                                   const AssetRowInfo &row) const
{
  bool hit = false;
  switch (c.src.dim)
  {
    case FilterDim::Search:
    {
      // 旧语义逐位对齐：空搜索词 = 命中；命中范围 displayName/effectiveType/
      // 关联（逗号拼接）/fileName，大小写不敏感 contains。
      hit = c.needle.isEmpty() ||
            row.displayName.contains(c.needle, Qt::CaseInsensitive) ||
            row.effectiveType.contains(c.needle, Qt::CaseInsensitive) ||
            row.entityNames.join(QLatin1Char(','))
                .contains(c.needle, Qt::CaseInsensitive) ||
            row.fileName.contains(c.needle, Qt::CaseInsensitive);
      break;
    }
    case FilterDim::Type:
      hit = row.effectiveType.compare(c.src.value, Qt::CaseInsensitive) == 0;
      break;
    case FilterDim::Status:
      hit = row.status.compare(c.src.value, Qt::CaseInsensitive) == 0;
      break;
    case FilterDim::Role:
      hit = row.roles.contains(c.src.value, Qt::CaseInsensitive); // 空值零命中（旧口径）
      break;
    case FilterDim::Entity:
      hit = row.entityNames.contains(c.src.value, Qt::CaseInsensitive);
      break;
    case FilterDim::Tag:
      hit = row.tags.contains(c.src.value, Qt::CaseInsensitive);
      break;
    case FilterDim::Regex:
    {
      if (c.src.value.isEmpty())
      {
        hit = true;
        break;
      }
      hit = c.re.match(row.fileName).hasMatch() || c.re.match(row.displayName).hasMatch();
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
  return c.src.negate ? !hit : hit;
}

bool CompiledQuery::matchColumns(const AssetRowInfo &row) const
{
  for (const CompiledColumn &col : m_cols)
  {
    bool any = false;
    for (const QString &raw : col.raw)
      if (columnRowHasValue(col.key, row, raw))
      {
        any = true;
        break;
      }
    if (!any)
      return false; // 空集：无 any → 不匹配（显式空集合语义）
  }
  return true;
}

bool CompiledQuery::matches(const AssetRowInfo &row) const
{
  if (m_empty)
    return true;
  if (!m_conds.isEmpty())
  {
    if (m_orMode)
    {
      bool any = false;
      for (const CompiledCondition &c : m_conds)
        if (matchCondition(c, row))
        {
          any = true;
          break;
        }
      if (!any)
        return false;
    }
    else
    {
      for (const CompiledCondition &c : m_conds)
        if (!matchCondition(c, row))
          return false;
    }
  }
  if (!m_tagNeedle.isEmpty() &&
      !row.tags.contains(m_tagNeedle, Qt::CaseInsensitive))
    return false;
  return matchColumns(row);
}

QList<CompiledQuery::ConditionSummary> CompiledQuery::summaries() const
{
  return m_summaries;
}

// ---- 状态串 v2 ------------------------------------------------------------------

namespace
{
// 维度键反查（filterDimKey 的逆；v1 解析器内联同名语义）。
bool filterDimFromKey(const QString &key, FilterDim *out)
{
  const struct
  {
    const char *k;
    FilterDim d;
  } kDims[] = {
      {"q", FilterDim::Search},           {"type", FilterDim::Type},
      {"status", FilterDim::Status},      {"role", FilterDim::Role},
      {"entity", FilterDim::Entity},      {"tag", FilterDim::Tag},
      {"re", FilterDim::Regex},           {"unlinked", FilterDim::Unlinked},
      {"unknownType", FilterDim::UnknownType}, {"warned", FilterDim::Warned},
  };
  for (const auto &m : kDims)
    if (key == QLatin1String(m.k))
    {
      if (out)
        *out = m.d;
      return true;
    }
  return false;
}
// 载荷编码：base64url（字母表 [A-Za-z0-9_-]，QUrl 查询值零干预往返）。
// 复审修正：原先的 '%'/'~' 手工转义会被 QUrlQuery 的 percent-语义二次
// 解码（值含 %XX 形子串损坏）、col 载荷含 '~' 被分隔符拆散——base64url
// 彻底绕开两层编码的相互作用，任意值（字面 !/中文/空格/#/&/%/+/~/空串）
// 逐字节保真。
QString encodePart(const QString &s)
{
  return QString::fromLatin1(s.toUtf8().toBase64(
      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
QString decodePart(const QString &s)
{
  return QString::fromUtf8(QByteArray::fromBase64(
      s.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
// 载荷合法性：可 base64url 解码（字符集 + 长度）且解码结果为合法 UTF-8。
// 坏载荷拒收（不静默解码成乱码——复审残留 (a) 收口）。
bool isValidPart(const QString &s)
{
  if (s.isEmpty())
    return true;
  for (const QChar &ch : s)
  {
    const char16_t u = ch.unicode();
    if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
          (u >= '0' && u <= '9') || u == '-' || u == '_'))
      return false;
  }
  const auto decoded = QByteArray::fromBase64Encoding(
      s.toLatin1(), QByteArray::Base64UrlEncoding |
                        QByteArray::OmitTrailingEquals |
                        QByteArray::AbortOnBase64DecodingErrors);
  if (decoded.decodingStatus != QByteArray::Base64DecodingStatus::Ok)
    return false;
  return QString::fromUtf8(decoded.decoded).toUtf8() == decoded.decoded;
}
} // namespace

QString serializeQuery(const DataQuery &q)
{
  QUrlQuery uq;
  uq.addQueryItem(QStringLiteral("v"), QStringLiteral("2"));
  uq.addQueryItem(QStringLiteral("op"), q.group.orMode ? QStringLiteral("or")
                                                       : QStringLiteral("and"));
  for (const FilterCondition &c : q.group.conditions)
    uq.addQueryItem(
        QStringLiteral("f"),
        QStringLiteral("%1~%2~%3")
            .arg(filterDimKey(c.dim), c.negate ? QStringLiteral("n") : QStringLiteral("p"),
                 encodePart(c.value)));
  if (!q.activeTag.isEmpty())
    uq.addQueryItem(QStringLiteral("tag"), encodePart(q.activeTag));
  for (const ColumnConstraint &col : q.columns)
  {
    QStringList parts;
    parts << columnKeyToken(col.key);
    for (const QString &v : col.values)
      parts << encodePart(v);
    uq.addQueryItem(QStringLiteral("col"), parts.join(QLatin1Char('~')));
  }
  QUrl url;
  url.setScheme(QStringLiteral("paleo"));
  url.setHost(QStringLiteral("dataops-filter"));
  url.setQuery(uq);
  return url.toString(QUrl::FullyEncoded);
}

QueryStateParse parseQueryStateString(const QString &s)
{
  QueryStateParse out;
  const QUrl url(s);
  if (s.trimmed().isEmpty() || !url.isValid())
  {
    out.diagnostic = QObject::tr("不是有效的过滤器链接");
    return out;
  }
  if (url.scheme() != QStringLiteral("paleo") ||
      url.host() != QStringLiteral("dataops-filter"))
  {
    out.diagnostic = QObject::tr("只支持 paleo://dataops-filter 链接（收到 %1://%2）")
                         .arg(url.scheme(), url.host());
    return out;
  }
  const QUrlQuery uq(url);
  const QString v = uq.queryItemValue(QStringLiteral("v"));
  if (v == QStringLiteral("2"))
  {
    // v2：未知字段/损坏格式一律拒收——丢字段静默应用更宽查询是安全事故。
    DataQuery q;
    const QString op = uq.queryItemValue(QStringLiteral("op"));
    if (!op.isEmpty() && op != QLatin1String("and") && op != QLatin1String("or"))
    {
      out.diagnostic = QObject::tr("状态串的组合方式字段损坏（%1）").arg(op);
      return out;
    }
    q.group.orMode = op == QLatin1String("or");
    const QList<QPair<QString, QString>> items = uq.queryItems(QUrl::FullyDecoded);
    for (const auto &kv : items)
    {
      const QString &key = kv.first;
      const QString &val = kv.second;
      if (key == QLatin1String("v") || key == QLatin1String("op"))
        continue;
      if (key == QLatin1String("f"))
      {
        const QStringList parts = val.split(QLatin1Char('~'));
        if (parts.size() != 3 || parts.at(1) != QLatin1String("p") &&
                                     parts.at(1) != QLatin1String("n"))
        {
          out.diagnostic = QObject::tr("状态串的过滤条件字段损坏");
          return out;
        }
        FilterDim d;
        if (!filterDimFromKey(parts.at(0), &d))
        {
          out.diagnostic = QObject::tr("状态串包含未知过滤维度「%1」").arg(parts.at(0));
          return out;
        }
        if (!isValidPart(parts.at(2)))
        {
          out.diagnostic = QObject::tr("状态串的过滤条件字段损坏");
          return out;
        }
        FilterCondition c;
        c.dim = d;
        c.negate = parts.at(1) == QLatin1String("n");
        c.value = decodePart(parts.at(2));
        q.group.conditions.append(c);
        continue;
      }
      if (key == QLatin1String("tag"))
      {
        if (!isValidPart(val))
        {
          out.diagnostic = QObject::tr("状态串的标签字段损坏");
          return out;
        }
        q.activeTag = decodePart(val);
        continue;
      }
      if (key == QLatin1String("col"))
      {
        const QStringList parts = val.split(QLatin1Char('~'));
        ColumnKey k;
        if (parts.isEmpty() || !columnKeyFromToken(parts.at(0), &k))
        {
          out.diagnostic = QObject::tr("状态串包含未知列「%1」")
                               .arg(parts.isEmpty() ? QString() : parts.at(0));
          return out;
        }
        QStringList values;
        for (int i = 1; i < parts.size(); ++i)
        {
          if (!isValidPart(parts.at(i)))
          {
            out.diagnostic = QObject::tr("状态串的列约束字段损坏");
            return out;
          }
          values << decodePart(parts.at(i));
        }
        q.setColumnValues(k, values); // 空表 = 显式空集合，语义保真
        continue;
      }
      out.diagnostic = QObject::tr("状态串包含未知字段「%1」，已拒绝应用").arg(key);
      return out;
    }
    out.valid = true;
    out.query = q;
    return out;
  }
  if (!v.isEmpty())
  {
    out.diagnostic = QObject::tr("不支持的状态串版本「%1」（当前支持 v1/v2）").arg(v);
    return out;
  }
  // 无 v 字段 = 旧 v1：按既定语义读取（值前缀 ! 即 negate——字面 ! 开头的
  // 旧值无法区分，如实按旧规则解释；未知字段按旧行为略过）。
  const FilterGroup g = FilterGroup::fromStateString(s);
  out.valid = true;
  out.legacy = true;
  out.query.group = g;
  return out;
}

// ---- 预设 ----------------------------------------------------------------------

bool saveQueryPreset(const QString &name, const DataQuery &q)
{
  const QString n = name.trimmed();
  if (n.isEmpty() || n.size() > 40)
    return false;
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  QVariantMap m = s.value(QStringLiteral("dataops/filterPresets")).toMap();
  m.insert(n, serializeQuery(q));
  s.setValue(QStringLiteral("dataops/filterPresets"), m);
  return true;
}

QueryStateParse loadQueryPreset(const QString &name)
{
  QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
  const QVariantMap m = s.value(QStringLiteral("dataops/filterPresets")).toMap();
  return parseQueryStateString(m.value(name).toString());
}

} // namespace paleo::dataops::query
