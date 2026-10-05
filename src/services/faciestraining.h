// 层：数据
#pragma once
#include "algorithms/cluster/supervised.h"
#include "domain/faciesclassification.h"
namespace paleo::crossplot {
inline bool isSupervisedClassifier(Classifier m) {
  return m == Classifier::Lda || m == Classifier::Qda || m == Classifier::Knn;
}
// 分类器枚举 → 监督算法枚举（仅 Lda/Qda/Knn 有效；其余按 Knn 兜底由调用方门禁拦截）。
inline cluster::SupervisedMethod supervisedMethodOf(Classifier m) {
  switch (m) {
  case Classifier::Lda:
    return cluster::SupervisedMethod::Lda;
  case Classifier::Qda:
    return cluster::SupervisedMethod::Qda;
  default:
    return cluster::SupervisedMethod::Knn;
  }
}
struct TrainingPrecondition {
  bool ok = false;
  QString error;              // 拒绝原因（中文，含实际类数/样本数）
  QStringList warnings;       // 非阻断提示（类样本失衡/小类）
  int classCount = 0;
  int labeledCount = 0;
};
struct TrainedModel {
  cluster::SupervisedModel model;
  // 平行 model.classIds 的类名（下标 = classIds 下标，稠密化后）。
  QStringList classNames;
  // 训练 provenance：method（字符串族名 "lda"/"qda"/"knn"）、supervisedMethod
  // （int(Classifier)，5/6/7 = Lda/Qda/Knn）、标准化 mean/sd（键
  // normalizationMean/normalizationSd + standardize）、trainingSetHash
  // （(行号,类码) 成对 + 类名 SHA-256）、labelProvenance、labeledCount、
  // classCount、sampleValuesHash、parameterHash（训练指纹，推理期保留为
  // trainingParameterHash 并另算推理层 parameterHash——两键分开）等。
  // 推理侧按这里的 standardize 标志与统计量还原口径，不按当前样本重估。
  QVariantMap provenance;
  cluster::ConfusionMatrix cv; // 分层 k-fold 交叉验证混淆矩阵（训练折内留出）
  int cvFolds = 0;
};
class FaciesTrainingService {
public:
  static TrainingPrecondition validateTrainingSet(const SampleSet &,
                                                  const TrainingSet &,
                                                  const ClassificationOptions &);
  // 训练 + 分层交叉验证。成功时 out 填模型与 provenance；失败时 error 带
  // 具体原因（中文）。ctl 可控取消（默认无）。
  static bool train(const SampleSet &, const TrainingSet &,
                    const ClassificationOptions &, TrainedModel *out,
                    QString *error, const cluster::Control &ctl = {});
  // 用已训练模型对全样本推理。labels 为类码（model.classIds[预测下标]），
  // confidence/squaredDistance 逐行平行 SampleSet。标准化口径一律取训练时
  // 记录（provenance 的 standardize + normalizationMean/Sd）：训练时标准化
  // 而统计量缺失即如实拒答，绝不按推理样本重估；o.standardize 与训练口径
  // 不一致时以训练口径为准并在 provenance 记 normalizationSource。
  static Classification classifyWith(const SampleSet &, const TrainedModel &,
                                     const ClassificationOptions &,
                                     const cluster::Control & = {});
};
} // namespace paleo::crossplot
