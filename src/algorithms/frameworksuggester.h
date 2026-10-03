// 层：数据
#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "../domain/sequenceframework.h"
#include "../domain/wellrecords.h"

// algorithms/ — 井间格架建议核（方向 28 目标 3，纯数值、无 Qt/GIS 依赖）。
//
// 问题：一口井的某个分层该归到哪个格架单元？人工逐层点选是编图前最耗时
// 的一步。本核只做「最近邻一致性」——用该单元已确认成员在各井上的深度区间
// 与厚度做参照，把未归属的井分层按归一化距离派给最近的单元。
//
// 为什么按「区间」而不是「顶深均值」：同一单元的不同成员（层序内的若干分层）
// 本身就跨越单元厚度（百米级），而同一层在各井之间的构造起伏只有十米级——
// 若按成员顶深的离散度归一化，单元内的正常成员会被判成超阈值。区间法语义
// 直白：落在单元的已确认深度区间内 = 距离 0，落在区间外 = 到最近边界的
// 距离（按区间跨度归一化）。
//
// 为什么优先用「本井区间」：构造起伏让绝对深度逐井漂移。只要该井里有这个
// 单元的已确认成员，就用本井区间做参照；该井没有成员时才退回全工区均值。
//
// 纪律（硬）：**建议只出候选，人工确认后才写库**。本文件是纯函数，既不接
// catalog 也不接文件——调用链上唯一的写库入口是
// services/frameworkservice.cpp 的 commitAccepted()，且只有 accepted 的候选
// 会落到格架上。未确认前 catalog 零写入（由 tst_sequenceframework 断言）。
//
// 归一化尺度一律取 max(实测离散度, 相对下限, 1.0)，绝不做除零。
namespace SequenceFramework
{

// 一口井一个分层的观测（顶深 + 到下一层顶的厚度）。
struct WellLayerObservation
{
  QString wellName;
  QString layerName;
  double topMd = 0.0;
  double thickness = 0.0;
  bool hasTop = false;
  bool hasThickness = false;
};

// 由井分层记录构造观测序列：同井内按顶深升序，厚度 = 下一层顶 − 本层顶；
// 同井最后一层（无下一层）hasThickness=false。无深度的行不产出观测。
// 输出按 (wellName, topMd) 稳定排序——同一输入必得同一输出。
QVector<WellLayerObservation> buildObservations( const QVector<WellTopRecord> &tops );

// 单元已确认成员在某口井里的深度区间（顶=最浅成员顶深，底=最深成员底深）。
struct UnitInterval
{
  double top = 0.0;
  double base = 0.0;
  bool valid = false;
  double span() const { return base - top; }
};

// wellName → unitId → 区间。井里没有该单元成员时不建条目（调用方退回全局）。
using WellIntervalTable = QHash<QString, QHash<QString, UnitInterval>>;

// 全工区口径的单元签名（本井无成员时的回落参照 + 厚度参照）。
struct UnitSignature
{
  QString unitId;
  double topRef = 0.0;          // 各井最浅成员顶深的均值
  double baseRef = 0.0;         // 各井最深成员底深的均值
  double thicknessRef = 0.0;    // 成员厚度均值
  double spanScale = 1.0;       // 归一化尺度 = max(区间跨度, 1.0)
  double thicknessScale = 1.0;  // = max(厚度离散度, 0.5|thicknessRef|, 1.0)
  int sampleCount = 0;          // 参与统计的（井,成员）样本数
  bool valid() const { return sampleCount > 0; }
};

// 已确认归属 = 标志层引用的分层名 → 所属单元（同名被多单元引用时取首个，
// 重名由诊断面另报）。
QHash<QString, QString> confirmedOwners( const Framework &fw );

WellIntervalTable buildWellIntervals( const Framework &fw,
                                      const QVector<WellLayerObservation> &obs );
QVector<UnitSignature> buildSignatures( const Framework &fw,
                                        const QVector<WellLayerObservation> &obs );

struct SuggestOptions
{
  double maxDistance = 1.0;      // 归一化距离上限；超出不出候选（诚实失败）
  double depthWeight = 1.0;      // 深度项权重
  double thicknessWeight = 1.0;  // 厚度项权重
  bool skipAlreadyAssigned = true; // 已在格架里归属过的分层不再建议
  bool requireThickness = false;   // true：无厚度的观测不出候选
};

struct SuggestionCandidate
{
  QString wellName;
  QString layerName;
  QString unitId;
  double distance = 0.0;       // 归一化距离，越小越可信
  double depthGap = 0.0;       // 到单元区间的距离（米；0 = 落在区间内）
  double thicknessDelta = 0.0; // 与单元代表厚度之差（米，带符号）
  bool usedWellInterval = false; // true = 参照本井区间；false = 退回全工区
  bool accepted = false;       // 人工确认标记；构造恒 false
};

// 主入口：最近邻建议。纯函数——同样的输入必得同样的输出（含顺序）。
QVector<SuggestionCandidate> suggestUnitAssignments(
    const Framework &fw, const QVector<WellLayerObservation> &obs,
    const SuggestOptions &opt = SuggestOptions() );

// 已确认候选（accepted==true）子集，保持原顺序。
QVector<SuggestionCandidate> acceptedCandidates( const QVector<SuggestionCandidate> &all );

// 应用已确认候选到格架：目标单元下已有同名标志层则并入（去重），否则新建
// 同名标志层。返回实际新建/并入的标志层数。**不写库**——写库由服务层负责。
int applyAccepted( Framework &fw, const QVector<SuggestionCandidate> &all );

} // namespace SequenceFramework
