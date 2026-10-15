// 层：数据
#pragma once
// G10b：跨算法统一「计算证据使用回执」的解释契约（纯 QtCore，无 QGIS/UI）。
//
// 三个不得混淆的概念：
//   consumed（本文件）——计算事实：哪些输入实际进了引擎、各占多少
//     （约束条数 / won_cells / 井数）。只有运行后的回执说了算。
//   SUPPORTED_BY——解释关系：地质上「这个结论由哪些证据支撑」，见
//     predictionevidence / interpretationevidence，与数值参与无关。
//   DERIVED_FROM——血缘：产物版本的父版本指针
//     （computational_parent_versions），回答「从哪来的」而非「用了多少」。
//
// normalize() 把任意成果版本 extra 里的各算法专业回执归一化为同一视图；
// 各算法保留原始字段，这里只是兼容查询层，不要求同构、不回写。
// 缺回执的旧版本 → legacy=true，如实标注，绝不编造参与记录。
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace paleo::receipts
{

// 归一化任意成果版本的 extra。返回结构（键恒定，值可为空——空列表/空 map
// 表示「该维度不适用或未记录」，不是「参与为零」）：
// {
//   known: bool              // kind 是否属于已知回执家族（含缺回执的旧版本）
//   kind: QString            // extra["kind"] 原样
//   declared_inputs:  [{version_id|id, layer_id?, title?, priority?}]
//                            // 计算「前」声明/计划的输入；无计划口径则空
//   accepted_inputs:  [...]  // 实际冻结并成功打开、进入引擎的输入（能确定时）
//   consumed_inputs:  [{id|version_id, ..., dimension?, consumed_count?,
//                       participation_scope?, role?}]
//                            // 计算事实：实际参与并影响结果的输入
//   skipped_inputs:   [{id|version_id, reason, dimension?}]
//                            // 未参与及原因（约束/样点/井各有 dimension）
//   effective_parameters: QVariantMap  // 实际生效的计算参数
//   ignored_parameters:  QVariantMap   // 声明了但被替换/忽略的参数（值=被忽略值）
//   result_version: QVariantMap        // 参与范围/产出规模（decided_cells 等）
//   quality_findings: [{severity, code, message}]
//   legacy: bool             // 早期版本无回执：诚实标注而不是伪造
// }
QVariantMap normalize( const QVariantMap &versionExtra );

// 计划与实际一致性比较。ConstraintUsePlan 是「计算前」的判定（UI 计数、
// 生成门用），receipts 是「运行后」的事实；二者对不上说明口径漂移，需要
// 显式暴露而不是静默吞掉。
//
// plan 宽容解析以下快照形状（按实际存在的键取用）：
//   {decisions: [{id, planned: "available|consumed|blocking"}]}
//   {decisions: [{id, consumed: bool, blocking: bool}]}   // Decision::toVariant
//   {consumed: [id...], unused: [{id, reason}...]}        // summary 快照
//   {consumed: [id...], available: [id...], blocking: [id...]}
//
// 返回 {comparable: bool, consistent: bool,
//       differences: [{id, planned, actual, reason}], note: QString}
// 未运行（normalized 为 legacy 或无 constraints 维度的 consumed/skipped）时
// comparable=false，note 明示「尚未运行，不能把计划当实际结果」。
QVariantMap compareWithPlan( const QVariantMap &normalizedReceipts, const QVariantMap &plan );

} // namespace paleo::receipts
