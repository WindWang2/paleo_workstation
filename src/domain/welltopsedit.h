// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

#include "wellrecords.h"

// domain/welltopsedit — 井分层编辑/质检的纯函数面（方向 32）。
// 输入输出全是值类型、无共享态：编辑表校验、批量差异预览、导入合并取舍
// 都可在任意线程调用；文件读写与 catalog 版本编排归
// workflow/welltopseditorworkflow（GUI 线程单事务）。
//
// 数据口径：DC.dat 是点式分层——每行 = 一个层位顶点（层名 + 深度列），
// 无底深列。层间区间由相邻行隐含，「顶<底」类校验映射为同井 MD 严格
// 递增（同深 = 叠置）与 TVD≤MD（域冲突）。
namespace WellTopsEdit
{

// ---- 校验 ------------------------------------------------------------
// issue 一律行级锚定（rowIndex = 行集内的 0 基行号），不汇总模糊报错。
enum class IssueKind
{
  EmptyName,      // 层名为空（错误）
  WhitespaceName, // 层名含空白（错误：DC.dat 按空白分列，含空白的层名写盘即损坏）
  EmptyWellName,  // 井名为空（错误：写盘后行归属无法解析）
  SentinelValue,  // 数值列命中共享缺失哨兵词表（错误：缺失应清空单元格）
  HalfCoordinateGroup, // X/Y 只有一列有效（警告：三列同组，缺列按哨兵落盘）
  DuplicateName, // 同井同层名多行——叠置（错误）：每个重复行各一条
  SameDepth,     // 同井两行 MD 相同——叠置（错误）：两行各一条
  TvdOverMd,     // TVD > MD——深度域冲突（错误）
  MdOutOfRange,  // MD < 0 或超过井 TD（错误）
  Inversion,     // 层序倒置：格架浅层位 MD 深于深层位（错误）：两行各一条
  DanglingName,  // 层名不在格架词表——悬空层名（警告：名单外层位合法存在）
  MissingTop     // 格架层位缺失——空洞（警告：锚到按层序应插入处的相邻行）
};

struct Issue
{
  IssueKind kind = IssueKind::EmptyName;
  int rowIndex = -1;  // 行级定位；MissingTop 锚相邻行，全井无格架行时 -1
  QString message;    // 面向用户的描述（含层名/深度等要素）
  bool isError() const;
};

QString issueKindLabel(IssueKind kind);

struct ValidationContext
{
  bool hasTd = false;   // 井 TD 已知（well_head 实体）
  double td = 0.0;
  QStringList framework; // 浅→深有序层序词表（AreaRules.sequenceBoundaries）
                          // 空 = 无格架：Inversion/DanglingName/MissingTop 跳过
};

QVector<Issue> validate(const QVector<WellTopRecord> &rows, const ValidationContext &ctx);

// 数值显示/写侧共用格式：3..9 位小数里最短的往返精确表示（负零归一）。
// 编辑表单元格与 DC.dat 写侧同用一个出口——显示层不得截断写侧精度。
QString formatDepth(double v);

// ---- 差异 ------------------------------------------------------------

struct DiffSummary
{
  int added = 0;
  int removed = 0;
  int changed = 0;
  bool isEmpty() const { return added == 0 && removed == 0 && changed == 0; }
};

// 按键（规范化井名 + 规范化层名）比对两份行集。同键两行且任一字段不同
// 计 changed；仅旧/仅新计 removed/added。键内重复行以首见为准（数据本就
// 异常，校验器另行报 DuplicateName）。
DiffSummary diff(const QVector<WellTopRecord> &oldRows, const QVector<WellTopRecord> &newRows);

// ---- 批量修正（跨井，纯变换；调用方拿返回计数做预览/确认）------------

// 层名重命名/统一：topName 规范化相等（去空白、忽略大小写）的行改名为 to。
// 返回改名行数。to 为空或与 from 规范化相等 → 0（不改）。
int applyRename(QVector<WellTopRecord> *rows, const QString &from, const QString &to);

// 基准整体位移：MD/TVD/Z 同加 delta（仅相应 has* 为真的列；坐标与时间
// 不动）。位移后越界不在变换里拦——留给校验器逐行报 MdOutOfRange。
int applyShift(QVector<WellTopRecord> *rows, double delta);

// 按层删除：topName 规范化相等的行全部移除。返回删除行数。
int applyDelete(QVector<WellTopRecord> *rows, const QString &topName);

// ---- 导入融合（差异对比 + 逐行取舍，不默认覆盖）------------------------

struct MergeRow
{
  enum class Resolution
  {
    KeepOld,  // 保留旧值（冲突默认；对「仅新」行 = 跳过新增）
    TakeNew,  // 采用新值（「仅新」行默认 = 新增；对「仅旧」行 = 不允许）
    RemoveOld // 删除旧行（仅「仅旧」行允许）
  };
  QString topName;
  bool inOld = false;
  bool inNew = false;
  WellTopRecord oldRec;
  WellTopRecord newRec;
  Resolution resolution = Resolution::KeepOld; // 诚实面：冲突默认不覆盖

  bool conflicts() const; // 同键两行且字段有差
};

// 旧=当前库内该井行，新=再导入文件中该井行。行序按旧序在前、仅新行按新序
// 追加。合并行默认值：冲突→KeepOld；仅旧→KeepOld；仅新→TakeNew（新增）。
QVector<MergeRow> mergeDiff(const QVector<WellTopRecord> &oldRows,
                            const QVector<WellTopRecord> &incoming);

// 应用取舍：KeepOld→emit oldRec（无旧行则不 emit）；TakeNew→emit newRec
// （无新行则不 emit）；RemoveOld→不 emit。输出行序 = 合并行序。
QVector<WellTopRecord> applyMerge(const QVector<MergeRow> &rows);

// 两行全字段相等（含 has* 标志；井名按规范化键比对）。
bool sameValues(const WellTopRecord &a, const WellTopRecord &b);

// 同井语境下的行相等（不比对井名）——编辑表的脏行/脏判定用：表的井上下文
// 由外层 combo 决定，切井瞬间 currentText 已是新井而基线还是旧井。
bool sameTop(const WellTopRecord &a, const WellTopRecord &b);

} // namespace WellTopsEdit
