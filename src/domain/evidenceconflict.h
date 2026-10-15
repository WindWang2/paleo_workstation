// 层：数据
#pragma once
// G06：多源地质证据冲突分析（纯域层聚合器）。
//
// 输入是「意见」（observations）：调用方（workflow 层）收集的各来源对
// 「该图斑此处应是什么相」的表达，加上图斑归因重放快照与既有
// paleo::evidence::check() 的 findings。本模块只做分类、定级与表达——
// 陈述事实与风险，不伪造统一置信度数值；Opinion::weight 仅作展示，
// 不参与任何「分数合成」。known=false（缺数据/越界/nodata）与「意见是
// 相 X」严格区分：未知既不算一致也不算冲突。
//
// conflict_type 固定词表（conflictTypes()）：
//   data_quality                数据质量（层位不符/约束停用/几何分类变化）
//   broken_reference            证据引用失效（missing_source*/stale_source）
//   spatial_contradiction       明确空间矛盾（井/井段/地震意见有空间落点）
//   interpretation_contradiction 地质解释矛盾（人工结论之间/与人工图斑分类）
//   model_uncertainty           模型不确定性（归因 unresolved/部分输入缺失/无意见）
//   manual_dispute              人工争议（CONTRADICTED_BY/disputed 观察并入）
//   consistency_note            一致性说明（Info 级）
//
// severity 四级（severities()）：Blocking > Error > Warning > Info，
// 输出按此排序（同级保持产生顺序）。
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace paleo::evidenceconflict {

// 一方证据意见：一个来源对「该图斑此处应是什么相」的表达。
// known=false 表示该来源无有效意见（缺数据/越界/nodata）——必须与
// 「意见是相 X」严格区分，不能把未知当一致或冲突。
struct Opinion {
  QString sourceKind;   // "well"|"well_interval"|"prediction"|"single_factor"|
                        // "constraint"|"seismic"|"manual"|"other"
  QString label;        // 展示名（井名/图层名/约束 id）
  QString versionId;    // 意见来源版本（可空=无版本事实）
  QString detail;       // 补充（井段深度/阈值区间/距离）
  int faciesCode = 0;   // 0 = 无编码意见
  bool known = false;   // 是否构成有效相意见
  double weight = 1.0;  // 展示用（如像元占比），不参与「分数合成」
  QVariantMap extra;    // 来源附加上下文（决策分支、participating、horizon 等）
};

QStringList conflictTypes();
QStringList severities();

// 汇总分析：opinions + 图斑当前 facies_code + 归因快照 + 既有 check()
// findings（原样并入，severity 归一）。每条 finding：
//   {severity, code, message, target{feature_uid,horizon,version_id},
//    evidence{source,label,version_id}, conflict_type, basis, suggestion,
//    status:"open"}
//
// 多数方口径：known 意见按编码计票取众数；自动归因（status ok/unresolved）
// 下图斑编码自身计一票；平票先倾向图斑编码、再按首见顺序定序。人工
// （manual/无自动归因）时图斑不计票——与人工结论的矛盾单独走
// interpretation_contradiction，且已被少数方 finding 点名的意见不重复
// 产出人工冲突。status=unresolved 不视为人工：归因缺口由 model_uncertainty
// 表达。
QVariantList analyze(const QList<Opinion> &opinions, int targetFaciesCode,
                     const QVariantMap &target, const QVariantMap &attribution,
                     const QVariantList &existingFindings = {});

// 把图斑归因重放的 per-input 记录（inputs[]：single_factor 意见，
// weight = 该输入在图斑内的胜出占比，faciesCode = 其 code_distribution
// 众数）转为意见列表；inputs 为空而汇总 code_distribution 存在时产出一条
// 汇总意见；status=manual 时无自动意见，返回空表。
QList<Opinion> opinionsFromAttribution(const QVariantMap &attribution);

// severity 词表归一到本模块四级（大小写不敏感；warn/note/critical 等
// 别名映射；未知或空值落 Warning——宁可多显示，不可漏显示）。
QString normalizeSeverity(const QString &severity);
} // namespace paleo::evidenceconflict
