// 层：功能
#pragma once

#include <QString>
#include <QVariantMap>

#include "domain/stratigraphicunit.h"

class DataCatalog;
struct CatalogVersion;
namespace paleo::capability { class ExecuteRequest; }

// workflow/ — 单因素/约束插值链的稳定分析身份（P3，#972/#1004/#1005）。
// 引擎准备入口统一经 resolveAnalysisIdentity 把（可能来自 UI/批任务/Host 的）
// UnitSelection 冻结为 Data Service 实体核验过的身份，再派生：
//   · analysisLayerKey —— 分析成果键（factor.<key>.* / wellfactor.<key>.*），
//     只认 layerToken（subIntervalId / agg-<intervalId>），不回退界面名；
//   · constraintHorizon —— 约束/井点等编辑态资产归属键（父单元上界面，
//     constraints.<top>），由方案派生，禁止调用方拆显示字符串。
// 身份双语义在引擎内一次裁决，调用方不再各自约定「horizon 实参是哪种键」。
namespace paleo::constraint_identity
{

struct FrozenAnalysisIdentity
{
  paleo::strat::UnitSelection unit; // 冻结后（修订已解析）的选择；hasUnit=false 时无效
  bool hasUnit = false;             // false 仅见于「工程无任何已提交方案」的 legacy 工程
  QString analysisLayerKey;         // 成果键：layerToken；legacy 回落入口 horizon
  QString constraintHorizon;        // 约束/井点编辑键：单元上界面；legacy 回落入口 horizon

  // 冻结单元的参数形态（unitSelection 键）；legacy 工程返回空 Map。
  QVariantMap unitMap() const { return hasUnit ? paleo::strat::toMap( unit ) : QVariantMap(); }
  // 发布 extra 的身份字段（与 strat::AnalysisBinding::publishIdentity 同口径，
  // 但来自引擎冻结结果而非 UI 选择状态）。
  QVariantMap publishIdentity() const;
  // 把身份写入任务参数：unitSelection / analysisLayerKey / constraintHorizon。
  // 调用方自带的同名键被冻结结果覆盖（不信任调用方字符串）。
  void enrichParams( QVariantMap &params ) const;
};

// aggregate 裁决：成图/插值入口拒绝（正式新分析按 S1–S4 运行）；提取入口
// 维持既有显式汇总合同（allowAggregate=true 的历史行为）。
enum class AggregatePolicy
{
  Reject,
  Allow
};

// 在引擎准备入口解析稳定分析身份：
//   1. params 携带 unitSelection → freezeAnalysisUnit 实体核验（scheme/revision、
//      interval/sub 归属、aggregate 裁决），伪造/过期选择明确拒绝；
//   2. 无 unitSelection → 工程已有已提交地层方案即拒绝（无可选单元的 legacy
//      工程除外，与 syncAnalysisResults 的 legacy 工区口径一致）。
// 拒绝发生在任何数值/暂存/发布动作之前；成功时 out 携带冻结身份。
bool resolveAnalysisIdentity( const QString &projectDir, const QString &horizon,
                              const QVariantMap &params, AggregatePolicy aggregatePolicy,
                              FrozenAnalysisIdentity &out, QString *error = nullptr );

// 工程是否存在任何已提交地层方案（无 Data Service 工区视为 false——不存在
// 可选单元，正式分析只能走 legacy 键或先建方案）。
bool projectHasAnalysisScheme( const QString &projectDir );

// 调用方携带的父版本核验：每个 id 必须是 catalog 里真实存在的版本
//（versionById 命中）。空列表通过；空 id 或不存在的 id → 拒绝并给出
// 可见原因。不接受「工具 JSON 自称的版本授权」。
bool callerParentVersionsExist( DataCatalog *catalog, const QStringList &parentVersionIds,
                                QString *error = nullptr );

// ---- constraint.idw Host 链（P3/W4）---------------------------------------
//
// Host 冻结钩子（capabilityhostwiring freezeInput 的 constraint.idw 分支）在
// 请求进入意图摘要前完成的受信解析：单元来自宿主捕获的当前选择（不取信
// 工具 JSON），points/constraints 引用只把能解析到真实 catalog 版本的记为
// 父版本（解析不到不臆造——inline 数值输入按合同记录参数摘要，不伪造父对象）。
struct ConstraintIdwFrozenContext
{
  paleo::strat::UnitSelection unit; // 冻结后的当前选择（S1–S4；aggregate 拒绝）
  QStringList parentVersionIds;     // 实际解析到的受管输入版本
  // 规范化写入 request.inputs 的身份字段（宿主权威，覆盖调用方自带值）。
  QVariantMap identityInputs() const;
  // 发布 extra 的身份字段（成员谱系；来自宿主冻结结果）。
  QVariantMap publishExtra() const;
};

// requestedSubinterval：请求 inputs 里的 subinterval_id（可空）。非空且与
// 宿主当前选择的子层段不一致 → subinterval_mismatch（不允许借当前选择给
// 别的单元出数，也不允许旧字符串换归属）。失败返回错误码（constraint_idw_* /
// input_version_missing），成功时 out 就绪。
QString freezeConstraintIdwContext( const QString &projectDir, DataCatalog *catalog,
                                    const paleo::strat::UnitSelection &activeUnit,
                                    const QString &requestedSubinterval,
                                    const QString &pointsRef, const QString &constraintsRef,
                                    ConstraintIdwFrozenContext &out );

// 宿主 freeze 钩子的 constraint.idw 分支（capabilityhostwiring 只做指针/目录
// 接线，本函数承载全部受信逻辑，测试与组装根共用同一实现）：
// 解析→规范化写入 request.inputs（subinterval_id/unit_selection/scheme 字段/
// parent_version_ids，覆盖调用方自带值）。返回空=成功，否则为拒绝码。
QString freezeConstraintIdwRequest( paleo::capability::ExecuteRequest &request,
                                    const QString &projectDir, DataCatalog *catalog,
                                    const paleo::strat::UnitSelection &activeUnit );

// 把 points/constraints 引用解析为受管版本：显式 "catalog:<versionId>" 或
// 与某版本 extra.manifest_layer_id / layer_id 精确匹配的层键。解析不到返回
// 空（调用方不得据此臆造父对象）。
QString resolveManagedVersionRef( DataCatalog *catalog, const QString &reference );

} // namespace paleo::constraint_identity
