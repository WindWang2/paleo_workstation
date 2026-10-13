// 层：数据
#pragma once
#include "formationdominance.h"
#include "wellcompositemodel.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantList>

struct WellFaciesModel {
  QString id, name, version, formationGroup;
  QStringList curves, labels;
  int window = 1;
};
struct WellFaciesInput {
  QJsonArray rows;
  QString reason;
  bool ready() const { return reason.isEmpty() && !rows.isEmpty(); }
};
struct WellFaciesResult {
  QString jobId, modelName, modelVersion;
  QVector<WellComposite::TextInterval> intervals;
  WellComposite::CurveData confidence;
  QJsonObject response;
  QString group; // 本结果对应的地层组；合并结果为各组名以「、」连接
  // 各地层单位（组、段）的优势微相：由工作流按井的地层道统计，供「优势相」道
  // 显示并写入地层优势相矢量表；解析/合并本身不填。
  QVector<FormationDominance> dominant;
};
Q_DECLARE_METATYPE(WellFaciesResult)

// 逐地层组预测的一项：一个地层组 + 匹配该组的模型 + 该组井段的输入。
struct WellFaciesRun {
  QString group; // 地层组名；空 = 模型不限组（整井）
  WellFaciesModel model;
  WellFaciesInput input;
};
// 一口井的逐组预测计划。runs 都是输入已就绪的项，按井深自上而下；
// skipped 是「组名（原因）」——有模型但输入不可用、井上有组却没有模型、
// 与前一项井段重叠。runs 为空时 reason 给出总原因。
struct WellFaciesPlan {
  QVector<WellFaciesRun> runs;
  QStringList skipped;
  QString reason;
  bool ready() const { return !runs.isEmpty(); }
};

WellFaciesInput
prepareWellFaciesInput(const WellComposite::ComprehensiveWellData &data,
                       const WellFaciesModel &model);
bool parseWellFaciesResult(const QJsonObject &json, const QString &wellName,
                           const WellFaciesInput &input,
                           WellFaciesResult *result, QString *error);
// 预测按地层组进行：井上每个有模型的地层组各提交一次，模型取 formationGroup
// 与该组一致的那个（同组多个版本时 preferredModelId 优先，否则取列表首个）。
// 井上没有标注地层组、或没有任何组命中模型时，沿用整井口径：只用所选模型
// （preferredModelId，空则首个）预测它能用的井段，不让多个模型预测同一批点。
WellFaciesPlan planWellFaciesRuns(const WellComposite::ComprehensiveWellData &data,
                                  const QVector<WellFaciesModel> &models,
                                  const QString &preferredModelId);
// 编图按当前地层预测：先把曲线、岩性、分层裁到该地层的 [顶深, 底深)，
// 只匹配一个模型并提交一次；当前范围外的曲线缺口不影响本次输入。
WellFaciesPlan planWellFaciesFormationRun(const WellComposite::ComprehensiveWellData &data,
                                        const QVector<WellFaciesModel> &models,
                                        const QString &horizon);
// 把逐组返回的结果并成一份供显示/导出：区间按井深排序，置信度曲线按深度拼接。
// 只有一份时原样返回。
WellFaciesResult mergeWellFaciesResults(const QVector<WellFaciesResult> &parts);
// 冻结相 schema（编图请求携带的 facies 词表）内查标签的相码：name 或
// microfacies 命中即返回该 code。schema 外标签返回 0——调用方必须拒收该结果
// （按井跳过并注明原因），不得自增编码：编码取决于到达顺序，同一井重跑会得到
// 不同的 code，且未知标签无词表色标，属性表/图例无法解释。
int faciesCodeForLabel(const QVariantList &schema, const QString &label);
