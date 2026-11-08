// 层：视图
// ui/pages/dataops/dataopsquery — P2 统一查询状态：FilterGroup + 活动标签 +
// 列值约束的单一事实源；编译查询（CompiledQuery）把准备（正则编译/值规范化）
// 与逐行匹配分离——同一查询扫描 N 行不再构造 N 份等价正则。纯值模块
//（QtCore，无 QWidget），匹配输入是 dataopsmodel.h 的 AssetRowInfo 快照。
#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "dataopsfilter.h"
#include "dataopsmodel.h"

namespace paleo::dataops::query
{

// ---- 列面（漏斗约束的目标列；与表格/虚拟表列语义对应）----------------------
enum class ColumnKey
{
  Name,    // displayName
  Type,    // effectiveType（含用户改写）
  Entity,  // entityNames 逐值（不按拼接显示串比较）
  Tag,     // tags 逐值
  Status,  // RAW / DERIVED
  Size,    // 字节数显示串
  Version, // "v%1"
  Time,    // "yyyy-MM-dd"
};

QString columnKeyToken(ColumnKey k);           // 状态串/内部键（"name"…）
QString columnKeyLabel(ColumnKey k);           // 用户可见列名（tr）
bool columnKeyFromToken(const QString &token, ColumnKey *out);

// 行在某一列上的候选值（Entity/Tag 多值；其余单值；显示口径与表格一致）。
QStringList columnValuesOf(ColumnKey k, const AssetRowInfo &row);
// 行在某一列上是否命中值 v（Name/Type/Status/Entity/Tag 大小写不敏感，
// Size/Version/Time 按显示串精确）。
bool columnRowHasValue(ColumnKey k, const AssetRowInfo &row, const QString &v);

// ---- 列约束 ------------------------------------------------------------------
// 同列多值 OR；跨列 AND；values 空 = 显式空集合（零命中）——与「未设置」
//（无该约束）严格区分。values 存储时排序去重。
struct ColumnConstraint
{
  ColumnKey key = ColumnKey::Type;
  QStringList values;
  bool operator==(const ColumnConstraint &o) const
  {
    return key == o.key && values == o.values;
  }
  QString display() const; // 「列·类型: a / b」
};

// ---- 统一查询状态（P2 单一事实源）--------------------------------------------
// 语义：FilterGroup 结果 ∧ 活动标签 ∧ 各列约束（组内 AND/OR 与 negate 保留）。
struct DataQuery
{
  FilterGroup group;                        // D2 条件组（含 negate、AND/OR）
  QString activeTag;                        // 标签云激活（空 = 不限）
  QVector<ColumnConstraint> columns;        // 列值约束

  bool isEmpty() const
  {
    return group.conditions.isEmpty() && activeTag.isEmpty() && columns.isEmpty();
  }

  // ---- 条件编辑语义（W1）----------------------------------------------------
  // 重复条件（dim+value+negate 全等）规范化为单份；返回是否真的变更。
  bool addCondition(const FilterCondition &c);
  // 按身份删一条（首个匹配）——chip 单删不动同值其他条件。
  bool removeFirstMatching(const FilterCondition &c);
  // 按身份取反一条（首个匹配）——chip 取反不再意外翻转所有同值条件。
  bool toggleFirstMatching(const FilterCondition &c);
  // 旧搜索框/类型框的编辑规则：只改「首个正向条件」——值为空删该条正向
  // 条件；否则替换/新增。否定条件、其他维度、用户有意保留的同维度多值
  // 一律不动。返回是否变更。
  bool setPositiveValue(FilterDim d, const QString &value);
  // 快捷开关（未挂接/类型未知/有警告）：on=补一条正向条件（去重），
  // off=只移除首个正向条件（否定同维条件保留）。
  bool setSwitch(FilterDim d, bool on);
  // 是否存在该维正向条件（快捷按钮勾选态/旧控件镜像口径）。
  bool hasPositive(FilterDim d) const;
  // 清空全部（条件+标签+列约束）。
  void clearAll();

  // ---- 列约束编辑（W3）------------------------------------------------------
  const ColumnConstraint *column(ColumnKey k) const;
  void setColumnValues(ColumnKey k, const QStringList &values); // 空表=显式空集合
  void clearColumn(ColumnKey k);                                // 回到「不限」
  // 逻辑候选值：按「应用其他条件、排除本列当前约束」计算，并并入本列
  // 当前已选值（零命中的已选值仍可见可取消）；不触发任何求值副作用。
  QStringList columnCandidates(ColumnKey k, const QVector<AssetRowInfo> &rows) const;
};

// ---- 编译查询（W2）-----------------------------------------------------------
// 一次编译、多行消费：正则只编译一次/条件，比较值预规范化；匹配零磁盘、
// 零 catalog 回查。诊断/摘要与匹配共用同一准备过程——界面写的与执行的
// 是同一套规则。
class CompiledQuery
{
public:
  struct ConditionSummary
  {
    QString text;                // 用户可读描述
    bool regexLiteralFallback;   // 无效正则退字面量（如实呈现）
  };

  static CompiledQuery compile(const DataQuery &q);

  // 完整语义匹配（组 ∧ 标签 ∧ 列约束）。
  bool matches(const AssetRowInfo &row) const;
  // 仅组+标签（列约束候选值计算用——排除本列约束后其余仍生效）。
  bool matchesWithoutColumns(const AssetRowInfo &row) const;

  // 条件说明（含列约束/标签），供空态解释与状态呈现。
  QList<ConditionSummary> summaries() const;
  // 正则编译次数（证据面：随条件数变化，与行数无关）。
  int regexCompileCount() const { return m_regexCompiles; }
  bool isEmpty() const { return m_empty; }

private:
  struct CompiledCondition
  {
    FilterCondition src;
    QString needle;              // Search/Type/Status/Role/Entity/Tag 预规范化
    QRegularExpression re;       // Regex 维预编译
    bool regexLiteral = false;   // 无效模式退字面量
  };
  struct CompiledColumn
  {
    ColumnKey key = ColumnKey::Type;
    QSet<QString> norm;          // 大小写不敏感键（ci 列）；精确列存原串
    bool caseInsensitive = true;
    QStringList raw;             // 展示顺序（排序去重）
  };

  bool matchCondition(const CompiledCondition &c, const AssetRowInfo &row) const;
  bool matchColumns(const AssetRowInfo &row) const;

  QVector<CompiledCondition> m_conds;
  bool m_orMode = false;
  bool m_empty = true;
  QString m_tagNeedle;           // 预规范化（空 = 不限）
  QVector<CompiledColumn> m_cols;
  int m_regexCompiles = 0;
  QList<ConditionSummary> m_summaries;
};

// ---- 状态串 v2 与诊断（W4）--------------------------------------------------
// v2：paleo://dataops-filter?v=2&op=and&f=type~n~%E5%80%BC&tag=…&col=type~a~b
// 值与 negate 分开编码（f=<dim>~<p|n>~<转义值>，转义只处理 % 与 ~），
// 字面 !、中文、空格、#、&、%、+ 均保真；分享内容涵盖列约束与活动标签。
// 无 v 字段 = 旧 v1 格式（按其既定语义读取；旧格式固有的 ! 前缀歧义如实
// 保留，不做无损声明）。未知字段/版本/损坏格式 → 无效 + 诊断（不静默丢字段）。
struct QueryStateParse
{
  bool valid = false;
  bool legacy = false;           // true = 按 v1 语义解析成功
  DataQuery query;
  QString diagnostic;            // 失败原因（用户可读）
};

QString serializeQuery(const DataQuery &q);
QueryStateParse parseQueryStateString(const QString &s);

// ---- 预设（沿用 dataops/filterPresets 存储，不建第二套配置）------------------
// 存 v2 全量串（条件+标签+列约束）；旧入口 loadFilterPreset 读新预设时取其
// 条件组部分（列约束/标签需新入口 loadQueryPreset）。
bool saveQueryPreset(const QString &name, const DataQuery &q);
QueryStateParse loadQueryPreset(const QString &name);

} // namespace paleo::dataops::query
