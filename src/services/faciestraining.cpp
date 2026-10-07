// 层：数据
#include "faciestraining.h"
#include "crossplotsamples.h"
#include "faciesclassificationservice.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
namespace paleo::crossplot {
namespace {
Classification trainingFailed(const QString &e, bool cancel = false) {
  Classification r;
  r.error = e;
  r.cancelled = cancel;
  return r;
}
bool bad(QString *e, const QString &t) {
  if (e)
    *e = t;
  return false;
}
QVariantList trainingList(const std::vector<double> &v) {
  QVariantList out;
  for (double x : v)
    out << x;
  return out;
}
QVariantList trainingList(const std::vector<int> &v) {
  QVariantList out;
  for (int x : v)
    out << x;
  return out;
}
// 小端定长拼字节——训练指纹跨构建可复现（x86/ARM 同值）。
void appendLe(QCryptographicHash &hash, quint32 v) {
  for (int b = 0; b < 4; ++b) {
    const char byte = char((v >> (8 * b)) & 0xFFu);
    hash.addData(&byte, 1);
  }
}
// 训练集指纹：先写 labeled 数，再逐对写 (行号, 类码)（小端），最后类名
// 个数 + 每个类名的 UTF-8 长度前缀（小端）+ 字节。行绑定杜绝「同码序列
// 标在不同行」与「[0,-1] vs [-1,0]」碰撞。
QString trainingSetHash(const TrainingSet &t) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  quint32 labeled = 0;
  for (int code : t.labels)
    if (code >= 0)
      ++labeled;
  appendLe(hash, labeled);
  for (std::size_t i = 0; i < t.labels.size(); ++i) {
    if (t.labels[i] < 0)
      continue;
    appendLe(hash, quint32(i));
    appendLe(hash, quint32(t.labels[i]));
  }
  appendLe(hash, quint32(t.classNames.size()));
  for (const QString &name : t.classNames) {
    const QByteArray bytes = name.toUtf8();
    appendLe(hash, quint32(bytes.size()));
    hash.addData(bytes);
  }
  return QString::fromLatin1(hash.result().toHex());
}
QString sampleValuesHash(const SampleSet &s) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(QByteArrayView(reinterpret_cast<const char *>(s.values.data()),
                              qsizetype(s.values.size() * sizeof(double))));
  return QString::fromLatin1(hash.result().toHex());
}
QVariantMap referenceMetadata(const SampleSet &s) {
  QVariantMap params;
  params.insert("dimensions", s.names);
  params.insert("units", s.units);
  params.insert("sampling", s.samplingMetadata);
  params.insert("referenceRows", s.grid.rows);
  params.insert("referenceCols", s.grid.cols);
  params.insert("referenceCRS", s.grid.crs);
  QVariantList referenceTransform;
  for (double v : s.grid.transform)
    referenceTransform << v;
  params.insert("referenceTransform", referenceTransform);
  return params;
}
const char *methodName(Classifier m) {
  switch (m) {
  case Classifier::Lda:
    return "lda";
  case Classifier::Qda:
    return "qda";
  default:
    return "knn";
  }
}
} // namespace
TrainingPrecondition
FaciesTrainingService::validateTrainingSet(const SampleSet &s,
                                           const TrainingSet &t,
                                           const ClassificationOptions &o) {
  TrainingPrecondition p;
  if (!isSupervisedClassifier(o.method)) {
    p.error = QStringLiteral("当前分类方法不是监督分类器（LDA/QDA/kNN）");
    return p;
  }
  // 参数下限与 algorithms 层 validOptions/crossValidate 对齐——先中文拒答，
  // 别让英文算法错误溜到界面。
  if (o.cvFolds < 2) {
    p.error = QStringLiteral("交叉验证折数至少为 2（当前 %1）").arg(o.cvFolds);
    return p;
  }
  if (o.method == Classifier::Knn && o.knnNeighbors < 1) {
    p.error = QStringLiteral("kNN 近邻数至少为 1（当前 %1）").arg(o.knnNeighbors);
    return p;
  }
  QString validation;
  if (!CrossplotSamples::validate(s, &validation)) {
    p.error = validation;
    return p;
  }
  if (!s.rows()) {
    p.error = QStringLiteral("没有有效样本；无法训练");
    return p;
  }
  if (t.labels.size() != s.rows()) {
    p.error = QStringLiteral("标注行数 %1 与样本行数 %2 不一致")
                  .arg(qsizetype(t.labels.size()))
                  .arg(qsizetype(s.rows()));
    return p;
  }
  const auto classSlots = std::size_t(t.classNames.size());
  std::vector<int> codes;
  int labeled = 0;
  for (int code : t.labels) {
    if (code < 0)
      continue;
    if (code > 254) {
      p.error = QStringLiteral("类码 %1 超出分类栅格编码范围 0-254").arg(code);
      return p;
    }
    if (std::size_t(code) >= classSlots) {
      p.error = QStringLiteral("类码 %1 没有对应类名（共 %2 个类名）")
                    .arg(code)
                    .arg(qsizetype(classSlots));
      return p;
    }
    ++labeled;
    codes.push_back(code);
  }
  p.labeledCount = labeled;
  if (!labeled) {
    p.error = QStringLiteral("训练集无标注样本；请先套索标注至少两个类");
    return p;
  }
  std::sort(codes.begin(), codes.end());
  codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
  const auto classCount = int(codes.size());
  p.classCount = classCount;
  if (classCount < 2) {
    p.error =
        QStringLiteral("仅 %1 个已标注类（类码 %2，%3 个样本）；监督分类至少需要两个类")
            .arg(classCount)
            .arg(codes.front())
            .arg(labeled);
    return p;
  }
  // counts 按类名槽位（原始类码）分配——类码稀疏时按下标 k 索引会越界。
  std::vector<int> counts(classSlots, 0);
  for (int code : t.labels)
    if (code >= 0 && std::size_t(code) < classSlots)
      ++counts[std::size_t(code)];
  // 交叉验证每折留出时每类至少留 2 个（crossValidate 的最小类下限），
  // 三族同一把尺；QDA 另有协方差自由度下限（supervised.cpp 同口径）。
  const int cvFloor = 2;
  const auto d = std::size_t(s.names.size());
  for (int code : codes) {
    const auto n = counts[std::size_t(code)];
    if (n < cvFloor) {
      p.error = QStringLiteral("类「%1」仅有 %2 个样本；交叉验证要求每类至少 %3 个")
                    .arg(t.classNames[code])
                    .arg(n)
                    .arg(cvFloor);
      return p;
    }
    if (o.method == Classifier::Qda && n < int(d) + 1) {
      p.error = QStringLiteral("类「%1」仅有 %2 个样本；QDA 每类至少需要维度+1 = %3 个")
                    .arg(t.classNames[code])
                    .arg(n)
                    .arg(int(d) + 1);
      return p;
    }
  }
  if (o.method == Classifier::Lda && labeled - classCount < int(d)) {
    p.error = QStringLiteral(
                  "标注样本数 %1 减去类数 %2 得 %3，小于维度 %4——LDA 合并协方差的自由度不足")
                  .arg(labeled)
                  .arg(classCount)
                  .arg(labeled - classCount)
                  .arg(qsizetype(d));
    return p;
  }
  int largest = 0, smallest = labeled;
  for (int code : codes) {
    largest = std::max(largest, counts[std::size_t(code)]);
    smallest = std::min(smallest, counts[std::size_t(code)]);
  }
  if (double(largest) > 10.0 * double(smallest))
    // 展示口径与触发口径一致：一位小数比（10.7:1）。
    p.warnings << QStringLiteral("类样本数比 %1:1 失衡（最多 %2 个，最少 %3 个）")
                      .arg(double(largest) / double(smallest), 0, 'f', 1)
                      .arg(largest)
                      .arg(smallest);
  for (int code : codes)
    if (counts[std::size_t(code)] < 5)
      p.warnings << QStringLiteral("类「%1」仅有 %2 个样本，交叉验证波动可能偏大")
                    .arg(t.classNames[code])
                    .arg(counts[std::size_t(code)]);
  // 孤立类名（建了名没有样本）不拒——只提示，避免误伤有样本的类。
  for (int code = 0; std::size_t(code) < classSlots; ++code)
    if (!counts[std::size_t(code)])
      p.warnings << QStringLiteral("类名「%1」没有标注样本，已忽略")
                    .arg(t.classNames[code]);
  p.ok = true;
  return p;
}
bool FaciesTrainingService::train(const SampleSet &s, const TrainingSet &t,
                                  const ClassificationOptions &o,
                                  TrainedModel *out, QString *error,
                                  const cluster::Control &ctl) {
  if (!out)
    return bad(error, QStringLiteral("训练输出参数为空"));
  const auto precondition = validateTrainingSet(s, t, o);
  if (!precondition.ok)
    return bad(error, precondition.error);
  if (ctl.cancelled && ctl.cancelled())
    return bad(error, QStringLiteral("已取消"));
  const auto n = s.rows(), d = std::size_t(s.names.size());
  cluster::Matrix matrix{d, s.values};
  std::vector<double> mean, sd;
  bool cancelled = false;
  if (o.standardize &&
      !FaciesClassificationService::standardizeMatrix(matrix, ctl, mean, sd,
                                                      &cancelled))
    return bad(error, QStringLiteral("已取消"));
  cluster::SupervisedOptions options;
  options.method = supervisedMethodOf(o.method);
  options.knnNeighbors = o.knnNeighbors;
  auto trained = cluster::trainSupervised(matrix, t.labels, options, ctl);
  if (!trained.ok)
    return bad(error, QStringLiteral("训练失败：%1")
                          .arg(QString::fromStdString(trained.error)));
  auto cv = cluster::crossValidate(matrix, t.labels, options, o.cvFolds, o.seed,
                                   ctl);
  if (!cv.ok)
    return bad(error, QStringLiteral("训练失败：%1")
                          .arg(QString::fromStdString(cv.error)));
  TrainedModel model;
  model.model = std::move(trained.model);
  // classNames 稠密化到 model.classIds 下标；缺失类名按类码占位（validate 已
  // 拦截码越界，这里兜底手搓 TrainingSet）。
  model.classNames.clear();
  for (int code : model.model.classIds)
    model.classNames << (std::size_t(code) < std::size_t(t.classNames.size())
                             ? t.classNames[code]
                             : QStringLiteral("类%1").arg(code));
  model.cv = std::move(cv.confusion);
  model.cvFolds = cv.folds;
  QVariantMap params = referenceMetadata(s);
  // provenance 键类型口径（批 3 UI 消费面）：
  //   method          = 字符串族名（"lda"/"qda"/"knn"）——train 与
  //                     classifyWith 同型同值；
  //   supervisedMethod = int(Classifier)（domain 枚举序，5/6/7 = Lda/Qda/Knn）；
  //                     cluster::SupervisedMethod 与之同序但别混用——两者是
  //                     不同枚举，UI 侧一律读 supervisedMethod。
  params.insert("method", methodName(o.method));
  params.insert("supervisedMethod", int(o.method));
  params.insert("standardize", o.standardize);
  params.insert("varianceFloor", 1e-6);
  if (o.standardize) {
    params.insert("normalizationMean", trainingList(mean));
    params.insert("normalizationSd", trainingList(sd));
  }
  params.insert("knnNeighbors", model.model.knnNeighbors);
  params.insert("cvFolds", model.cvFolds);
  params.insert("cvFoldsRequested", o.cvFolds);
  // 交叉验证在已按全样本统计量标准化的矩阵上跑——各折沿用全样本 mean/sd
  // （直推式口径），不按折内重估。
  params.insert("cvStandardization", "full_sample_transductive");
  params.insert("seed", QString::number(o.seed));
  params.insert("classIds", trainingList(model.model.classIds));
  params.insert("classNames", model.classNames);
  params.insert("classCount", precondition.classCount);
  params.insert("labeledCount", precondition.labeledCount);
  params.insert("labelProvenance",
                t.provenance.value(QStringLiteral("labelProvenance"),
                                   QStringLiteral("free_text_lasso")));
  params.insert("trainingSetHash", trainingSetHash(t));
  params.insert("sampleValuesHash", sampleValuesHash(s));
  params.insert("sampleCount", qint64(n));
  params.insert("rejected", s.rejected);
  QVariantList precision, recall;
  for (double v : model.cv.precision)
    precision << v;
  for (double v : model.cv.recall)
    recall << v;
  params.insert("cvPrecision", precision);
  params.insert("cvRecall", recall);
  QVariantList cells;
  for (const auto &row : model.cv.cells) {
    QVariantList line;
    for (std::int64_t v : row)
      line << qint64(v);
    cells << line;
  }
  params.insert("cvCells", cells);
  const QByteArray json =
      QJsonDocument::fromVariant(params).toJson(QJsonDocument::Compact);
  params.insert(
      "parameterHash",
      QString::fromLatin1(
          QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex()));
  // 训练侧指纹口径：parameterHash 只哈希训练参数（含标准化统计量、CV 结果、
  // trainingSetHash）。推理期 FaciesTrainingService::classifyWith 会把它
  // 保留为 trainingParameterHash 并另算推理层 parameterHash——两键分开。
  model.provenance = params;
  *out = std::move(model);
  return true;
}
Classification
FaciesTrainingService::classifyWith(const SampleSet &s, const TrainedModel &m,
                                    const ClassificationOptions &o,
                                    const cluster::Control &ctl) {
  QString validation;
  if (!CrossplotSamples::validate(s, &validation))
    return trainingFailed(validation);
  if (!s.rows())
    return trainingFailed(QStringLiteral("没有有效样本；未生成分类"));
  if (ctl.cancelled && ctl.cancelled())
    return trainingFailed(QStringLiteral("已取消"), true);
  const auto n = s.rows(), d = std::size_t(s.names.size());
  if (m.model.dimensions != d)
    return trainingFailed(QStringLiteral("模型维度 %1 与样本维度 %2 不一致；请用当前样本重新训练")
                      .arg(qsizetype(m.model.dimensions))
                      .arg(qsizetype(d)));
  if (m.classNames.size() != qsizetype(m.model.classIds.size()))
    return trainingFailed(QStringLiteral("模型类名数与类别数不一致"));
  if (!isSupervisedClassifier(o.method))
    return trainingFailed(QStringLiteral("当前分类方法不是监督分类器（LDA/QDA/kNN）"));
  if (supervisedMethodOf(o.method) != m.model.method)
    return trainingFailed(QStringLiteral("模型方法与当前分类方法不一致；请重新训练"));
  // 标准化口径一律取训练时记录：先要 standardize 标志（缺失即拒，手搓模型
  // 不放行），标志为真时强制要求落盘的 normalizationMean/Sd——绝不按推理
  // 样本重估（重估会让按原始量纲拟合的模型被强加标准化，标签漂移）。
  const auto flag = m.provenance.value(QStringLiteral("standardize"));
  if (!flag.isValid() ||
      flag.typeId() != QMetaType::Bool)
    return trainingFailed(QStringLiteral("模型 provenance 缺少标准化口径记录；请重新训练"));
  const bool modelStandardize = flag.toBool();
  cluster::Matrix matrix{d, s.values};
  QString normalizationSource;
  std::vector<double> mean(d, 0), sd(d, 1);
  if (modelStandardize) {
    const auto storedMean = m.provenance.value(QStringLiteral("normalizationMean"));
    const auto storedSd = m.provenance.value(QStringLiteral("normalizationSd"));
    if (!storedMean.isValid() || storedMean.typeId() != QMetaType::QVariantList ||
        !storedSd.isValid() || storedSd.typeId() != QMetaType::QVariantList)
      return trainingFailed(
          QStringLiteral("模型缺少标准化统计量（normalizationMean/Sd）；无法按训练口径推理，请重新训练"));
    const auto meanList = storedMean.toList();
    const auto sdList = storedSd.toList();
    if (meanList.size() != qsizetype(d) || sdList.size() != qsizetype(d))
      return trainingFailed(QStringLiteral("模型标准化统计量与样本维度不一致；请重新训练"));
    for (std::size_t j = 0; j < d; ++j) {
      const double m = meanList[qsizetype(j)].toDouble();
      const double v = sdList[qsizetype(j)].toDouble();
      if (!std::isfinite(m) || !std::isfinite(v) || !(v > 0))
        return trainingFailed(
            QStringLiteral("模型标准化统计量含非有限值或非正尺度；请重新训练"));
      mean[j] = m;
      sd[j] = v;
    }
    for (std::size_t i = 0; i < n; ++i) {
      if ((i & 1023U) == 0 && ctl.cancelled && ctl.cancelled())
        return trainingFailed(QStringLiteral("已取消"), true);
      for (std::size_t j = 0; j < d; ++j)
        matrix.values[i * d + j] =
            (matrix.values[i * d + j] - mean[j]) / sd[j];
    }
    normalizationSource = QStringLiteral("model_normalization_stats");
  } else {
    normalizationSource = QStringLiteral("model_training_raw_units");
  }
  auto predicted = cluster::predictSupervised(matrix, m.model, ctl);
  if (!predicted.ok)
    return trainingFailed(QString::fromStdString(predicted.error),
                  predicted.cancelled);
  Classification r;
  r.labels.resize(n);
  for (std::size_t i = 0; i < n; ++i)
    // 类码 = model.classIds 下标取值（稠密编码下与下标同值）。
    r.labels[i] = m.model.classIds[std::size_t(predicted.labels[i])];
  r.confidence = std::move(predicted.confidence);
  r.squaredDistance = std::move(predicted.squaredDistance);
  if (std::any_of(r.labels.begin(), r.labels.end(), [](int label) {
        return label < -1 || label > 254;
      }))
    return trainingFailed(QStringLiteral("类别编号超出分类栅格编码范围"));
  int classes = 0;
  for (int l : r.labels)
    classes = std::max(classes, l + 1);
  r.counts.fill(0, classes);
  for (int l : r.labels)
    if (l >= 0)
      ++r.counts[l];
  QVariantMap params = m.provenance;
  // 指纹分键：训练期 parameterHash 原样保留为 trainingParameterHash；推理层
  // parameterHash 只覆盖推理参数——不把训练指纹当输入再哈希（链式哈希会让
  // 「训练指纹」和「推理指纹」焊死成一个说不清的口径）。
  if (params.contains("parameterHash")) {
    params.insert("trainingParameterHash", params.value("parameterHash"));
    params.remove("parameterHash");
  }
  // 键类型同 train：method = 字符串族名；supervisedMethod = int(Classifier)
  // （m.provenance 里的同键原样覆盖，值恒等——模型本就是按 o.method 训练的，
  // 上面已校验一致）。
  params.insert("method", methodName(o.method));
  params.insert("supervisedMethod", int(o.method));
  params.insert("classNames", m.classNames);
  params.insert("confidenceMaskThreshold", o.confidenceMaskThreshold);
  params.insert("confidence", o.method == Classifier::Knn
                                  ? "winning_class_vote_share"
                                  : "maximum_plug_in_posterior");
  params.insert("normalizationSource", normalizationSource);
  if (bool(o.standardize) != modelStandardize)
    params.insert("standardizeRequestIgnored", true); // 以训练口径为准
  const QByteArray json =
      QJsonDocument::fromVariant(params).toJson(QJsonDocument::Compact);
  params.insert(
      "parameterHash",
      QString::fromLatin1(
          QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex()));
  params.insert("sampleValuesHash", sampleValuesHash(s));
  params.insert("sampleCount", qint64(n));
  params.insert("rejected", s.rejected);
  params.insert("origin", "crossplot_supervised");
  params.insert("geologicalMeaning",
                QStringLiteral("supervised_named_classes:") +
                    m.classNames.join(QStringLiteral(",")));
  r.provenance = params;
  r.ok = true;
  if (ctl.progress)
    ctl.progress(1);
  return r;
}
} // namespace paleo::crossplot
