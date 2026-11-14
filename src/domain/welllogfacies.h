// 层：数据
#pragma once
#include "formationdominance.h"
#include "wellcompositemodel.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantList>
#include <QMap>

struct WellFaciesModel {
  // formationGroup is descriptive metadata, not an applicability constraint.
  QString id, name, version, formationGroup;
  QStringList curves, labels;
  int window = 1;
  QMap<QString, QString> curveUnits{}; // Exact training feature units when the model contract declares them.
  QString contractReadinessCode; // Parser evidence for unsupported declared categorical/schema requirements.
};
struct WellFaciesInput {
  QJsonArray rows;
  QVariantList facies; // 冻结的三级分类；空时使用内置模板
  QStringList modelLabels; // 模型声明的全部输出类别；绑定后禁止接收集合外结果。
  QString reason;
  bool ready() const { return reason.isEmpty() && !rows.isEmpty(); }
};
struct WellFaciesResult {
  QString jobId, modelName, modelVersion;
  QVector<WellComposite::TextInterval> intervals;
  WellComposite::CurveData confidence;
  QJsonObject response;
  QVariantList facies; // 微相编码及完整父级归属
  QVector<WellComposite::FaciesInterval> hierarchyIntervals;
  QString group; // 本结果对应的地层组；合并结果为各组名以「、」连接
  // 各地层单位（组、段）的优势微相：由工作流按井的地层道统计，供「优势相」道
  // 显示并写入地层优势相矢量表；解析/合并本身不填。
  QVector<FormationDominance> dominant;
};
Q_DECLARE_METATYPE(WellFaciesResult)

// 逐地层组预测的一项：一个实际地层组 + 通用模型 + 该组井段的输入。
struct WellFaciesRun {
  QString group; // 实际地层组名；空 = 输入未标组（整井/当前分析窗）
  WellFaciesModel model;
  WellFaciesInput input;
};
// 一口井的逐组预测计划。runs 都是输入已就绪的项，按井深自上而下；
// skipped 是「组名（原因）」——输入不可用或与前一项井段重叠。
// runs 为空时 reason 给出总原因。
struct WellFaciesPlan {
  QVector<WellFaciesRun> runs;
  QStringList skipped;
  QString reason;
  bool ready() const { return !runs.isEmpty(); }
};

// 深度单位换算到米的系数：m/M/METER(S)/METRE(S) → 1，ft/F/FT/FEET/FOOT → 0.3048，
// 空单位按米；容忍首尾空白与 LAS 常见的尾随 '.'。无法识别返回 NaN。
double depthUnitToMeters(const QString &unit);
// 把 LAS 读出的（深度,值）对规范化成预测输入口径：按单位换算成米，丢弃非有限
// 深度（中间空深度），按深度稳定升序（兼容负 STEP 的递减 LAS），同深度保留
// 首个样点。单位无法识别或长度不一致时返回 false 并给出 reason。
bool normalizeDepthCurve(const QVector<double> &depths, const QVector<double> &values,
                         const QString &depthUnit, WellComposite::CurveData *curve,
                         QString *reason);

enum class WellFaciesSourcePolicy { Legacy, DeclaredSdkValues };
WellFaciesInput
// validSegments != nullptr: split at nulls/gaps and retain only runs meeting model.window.
// The strict default rejects discontinuous input; missing data is never filled.
prepareWellFaciesInput(const WellComposite::ComprehensiveWellData &data,
                       const WellFaciesModel &model,
                       QVector<WellFaciesInput> *validSegments = nullptr,
                       QStringList *skipped = nullptr,
                       bool includeFinalBottom = false,
                       WellFaciesSourcePolicy sourcePolicy = WellFaciesSourcePolicy::Legacy);
// 在提交或准备检查前冻结全部模型微相的三级归属，点名未知/歧义类别。
bool bindWellFaciesClasses(const WellFaciesModel &model, const QVariantList &schema,
                           WellFaciesInput *input, QString *error);
bool resolveWellFaciesClasses(const WellFaciesInput &input, const QStringList &labels,
                              QVariantList *schema, QString *error);
bool parseWellFaciesResult(const QJsonObject &json, const QString &wellName,
                           const WellFaciesInput &input,
                           WellFaciesResult *result, QString *error);
// 按井上实际地层组组织独立任务；所有模型均可用于所有组/段。
// 多模型时同名描述优先，同组多个版本 preferredModelId 优先；其余使用所选/默认
// 模型。没有组标注时按整个输入组织任务，同一个样点只预测一次。
WellFaciesPlan planWellFaciesRuns(const WellComposite::ComprehensiveWellData &data,
                                  const QVector<WellFaciesModel> &models,
                                  const QString &preferredModelId, bool allowPartial = false);
// 编图按当前地层预测：先把曲线、岩性、分层裁到该地层的 [顶深, 底深)，
// 只匹配一个模型并提交一次；当前范围外的曲线缺口不影响本次输入。
WellFaciesPlan planWellFaciesFormationRun(const WellComposite::ComprehensiveWellData &data,
                                        const QVector<WellFaciesModel> &models,
                                        const QString &horizon);
// 把逐组返回的结果并成一份供显示/导出：区间按井深排序，置信度曲线按深度拼接。
// 只有一份时原样返回。
WellFaciesResult mergeWellFaciesResults(const QVector<WellFaciesResult> &parts);
// 冻结三级 schema 内查微相标签的相码：完整路径 / 唯一名称 / 别名命中。
// 缺父级、歧义或 schema 外标签返回 0。调用方两种合法消费：
// 拒收该结果（按井跳过并注明原因），或做**确定性**扩展（先收集全部未知
// 标签、排序后统一分配新码）——不得边遍历边 max+1：编码会随到达顺序漂移，
// 同一标签集跨 run 得到不同码，属性表/图例不可比。
int faciesCodeForLabel(const QVariantList &schema, const QString &label);
