// 层：功能
#pragma once
#include "services/algorithmspec.h"
#include "unitonnxcontract.h"
#include <QJsonObject>
#include <QString>
#include <functional>

class PaleoOnnxService;

namespace paleo {

// compute-only 阶段的受控推理（任务线程）。只消费冻结计划：
// · 钉权重：runPinnedTensor(model, plan.modelSha256, …)，不依赖全局活动模型；
// · 输入：冻结 64×64 RMS 样本（NaN→0 喂入，输出端重施有效 mask）；
// · 首输出：[1,C,64,64] 分类合同（logits/probabilities），argmax 类索引，
//   类别只报告模型通道索引——无地学标签映射时不解释 0/1/2 的含义；
// · 产物：OUTPUT.tif（Byte，nodata=255）入返回值的 artifacts（正式输出集）；
//   DIAGNOSTICS.json 经 diagnostics 出参交给 OperationWorkResult.diagnostics
//   （宿主私有保留，发布时自动剔除）。
AlgorithmRunResult computeUnitOnnxPlan(PaleoOnnxService *onnx, const FrozenUnitOnnxPlan &plan,
                                       const QString &privateDir, AlgorithmRunResult &diagnostics,
                                       QJsonObject &summary, const std::function<bool()> &cancelled);

} // namespace paleo
