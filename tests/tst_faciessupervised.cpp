#include "catalog/datacatalog.h"
#include "catalog/purgelease.h"
#include "metadata/layermanifest.h"
#include "metadata/paleoprojectstore.h"
#include "qgis/qgislayerservice.h"
#include "qgis/qgisprojectservice.h"
#include "services/crossplotsamples.h"
#include "services/faciesclassificationservice.h"
#include "services/faciestraining.h"
#include "workflow/faciesclassify.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDirIterator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <gdal.h>
#include <limits>
#include <qgsapplication.h>
#include <random>
#include <vector>
using namespace paleo::crossplot;
namespace {
// gridRows×gridCols 全采样两类高斯夹具：相邻行不同类（交混，接近真实标注
// 顺序），separation 为每轴中心距（spread=σ，4σ 时 separation=4）。
SampleSet twoClassGrid(int gridRows, int gridCols, double spread,
                       double separation, std::uint64_t seed) {
  const int rows = gridRows * gridCols;
  SampleSet s;
  s.names << "A" << "B";
  s.units << "u" << "u";
  s.parentVersionIds << "raw-a" << "raw-b";
  s.grid.rows = gridRows;
  s.grid.cols = gridCols;
  s.grid.spatial = true;
  s.values.reserve(std::size_t(rows) * 2);
  s.locations.reserve(rows);
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> noise(0, spread);
  for (int i = 0; i < rows; ++i) {
    const int cls = i % 2;
    const double center = (cls ? 0.5 : -0.5) * separation;
    s.values.push_back(center + noise(rng));
    s.values.push_back(center + noise(rng));
    Location loc;
    loc.pixel = i;
    loc.hasXY = true;
    const double col = i % gridCols + .5, row = i / gridCols + .5;
    loc.x = s.grid.transform[0] + col * s.grid.transform[1] +
            row * s.grid.transform[2];
    loc.y = s.grid.transform[3] + col * s.grid.transform[4] +
            row * s.grid.transform[5];
    s.locations << loc;
  }
  return s;
}
// gridSide 全采样三类交叠夹具（半径 radius、各向同性 spread）：低置信区
// 充足，供掩膜 oracle 使用。
SampleSet threeClassGrid(int gridSide, double spread, double radius,
                         std::uint64_t seed) {
  const int rows = gridSide * gridSide;
  SampleSet s;
  s.names << "A" << "B" << "C";
  s.units << "u" << "u" << "u";
  s.parentVersionIds << "raw-a" << "raw-b";
  s.grid.rows = gridSide;
  s.grid.cols = gridSide;
  s.grid.spatial = true;
  s.values.reserve(std::size_t(rows) * 3);
  s.locations.reserve(rows);
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> noise(0, spread);
  for (int i = 0; i < rows; ++i) {
    const int cls = i % 3;
    constexpr double twoPi = 6.28318530717958647692;
    const double angle = twoPi * cls / 3;
    s.values.push_back(radius * std::cos(angle) + noise(rng));
    s.values.push_back(radius * std::sin(angle) + noise(rng));
    s.values.push_back(0.5 * cls + noise(rng));
    Location loc;
    loc.pixel = i;
    loc.hasXY = true;
    const double col = i % gridSide + .5, row = i / gridSide + .5;
    loc.x = s.grid.transform[0] + col * s.grid.transform[1] +
            row * s.grid.transform[2];
    loc.y = s.grid.transform[3] + col * s.grid.transform[4] +
            row * s.grid.transform[5];
    s.locations << loc;
  }
  return s;
}
// 半采样两类夹具：仅 pixels 覆盖半数像元，其余像元无样本。
SampleSet halfSampledGrid(const QVector<int> &pixels,
                          const QVector<QPair<double, double>> &values,
                          const QString &crs) {
  SampleSet s;
  s.names << "A" << "B";
  s.units << "u" << "u";
  s.parentVersionIds << "raw-a" << "raw-b";
  s.grid.rows = 4;
  s.grid.cols = 4;
  s.grid.spatial = true;
  s.grid.crs = crs;
  Q_ASSERT(pixels.size() == values.size());
  for (int i = 0; i < pixels.size(); ++i) {
    s.values.push_back(values[i].first);
    s.values.push_back(values[i].second);
    Location loc;
    loc.pixel = pixels[i];
    loc.hasXY = true;
    const double col = pixels[i] % s.grid.cols + .5,
                 row = pixels[i] / s.grid.cols + .5;
    loc.x = s.grid.transform[0] + col * s.grid.transform[1] +
            row * s.grid.transform[2];
    loc.y = s.grid.transform[3] + col * s.grid.transform[4] +
            row * s.grid.transform[5];
    s.locations << loc;
  }
  return s;
}
TrainingSet twoClassLabels(const SampleSet &s) {
  TrainingSet t;
  t.labels.assign(s.rows(), -1);
  for (std::size_t i = 0; i < s.rows(); ++i)
    t.labels[i] = int(i % 2);
  t.classNames << "砂岩" << "泥岩";
  t.provenance.insert("labelProvenance", "free_text_lasso");
  return t;
}
TrainingSet threeClassLabels(const SampleSet &s) {
  TrainingSet t;
  t.labels.assign(s.rows(), -1);
  for (std::size_t i = 0; i < s.rows(); ++i)
    t.labels[i] = int(i % 3);
  t.classNames << "砂岩" << "泥岩" << "灰岩";
  t.provenance.insert("labelProvenance", "free_text_lasso");
  return t;
}
paleo::cluster::SupervisedOptions supervisedOptionsFor(Classifier m) {
  paleo::cluster::SupervisedOptions o;
  o.method = supervisedMethodOf(m);
  return o;
}
// 用模型 provenance 记录的标准化口径还原矩阵（高 2 修复后无重估回退）；
// 口径不符/统计量缺失即 ok=false + 中文原因。
bool matrixWithModelNormalization(const SampleSet &s, const TrainedModel &m,
                                  paleo::cluster::Matrix *out, QString *why) {
  const auto d = std::size_t(s.names.size());
  const auto flag = m.provenance.value("standardize");
  if (!flag.isValid() || flag.typeId() != QMetaType::Bool) {
    *why = QStringLiteral("模型 provenance 缺少标准化口径记录");
    return false;
  }
  paleo::cluster::Matrix matrix{d, s.values};
  if (!flag.toBool()) {
    *out = matrix;
    return true;
  }
  const auto mean = m.provenance.value("normalizationMean").toList();
  const auto sd = m.provenance.value("normalizationSd").toList();
  if (mean.size() != qsizetype(d) || sd.size() != qsizetype(d)) {
    *why = QStringLiteral("模型标准化统计量缺失或维度不符");
    return false;
  }
  for (std::size_t i = 0; i < s.rows(); ++i)
    for (std::size_t j = 0; j < d; ++j)
      matrix.values[i * d + j] =
          (matrix.values[i * d + j] - mean[qsizetype(j)].toDouble()) /
          sd[qsizetype(j)].toDouble();
  *out = matrix;
  return true;
}
CatalogVersion derivedVersion(const DataCatalog &cat, const QString &type) {
  for (const auto &a : cat.assets())
    if (a.type == type) {
      const auto v = cat.currentVersion(a.id);
      if (v.stage == "DERIVED")
        return v;
    }
  return CatalogVersion{};
}
// 递归数受管派生产物目录下的文件数（staged 未登记件被清 → 计数回落）。
int countFiles(const QString &dir) {
  int n = 0;
  QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    ++n;
  }
  return n;
}
struct Rig {
  QTemporaryDir dir;
  DataCatalog cat;
  PaleoProjectStore store;
  PaleoTaskService tasks;
  QgisProjectService project;
  LayerManifest manifest;
  QgisLayerService layers;
  Rig()
      : tasks(&store), manifest(dir.filePath("metadata.sqlite")),
        layers(&project, &manifest) {}
};
// 训练 + 推理闭环 oracle（LDA/QDA/kNN 三族共用）：可分 4σ 数据，CV 每类
// precision/recall ≥ 0.9；classifyWith 与 algorithms 层逐位一致。
void trainInferenceOracle(Classifier method) {
  const SampleSet s = twoClassGrid(16, 8, 1, 4, 11);
  const TrainingSet t = twoClassLabels(s);
  ClassificationOptions o;
  o.method = method;
  const auto pre = FaciesTrainingService::validateTrainingSet(s, t, o);
  QVERIFY2(pre.ok, qPrintable(pre.error));
  TrainedModel m;
  QString error;
  QVERIFY2(FaciesTrainingService::train(s, t, o, &m, &error), qPrintable(error));
  QCOMPARE(m.classNames, (QStringList{"砂岩", "泥岩"}));
  QCOMPARE(m.model.classIds, (std::vector<int>{0, 1}));
  QCOMPARE(m.cvFolds, 5);
  for (int k = 0; k < 2; ++k) {
    QVERIFY2(m.cv.precision[std::size_t(k)] >= 0.9,
             qPrintable(QString("%1 precision[%2]=%3")
                            .arg(int(method))
                            .arg(k)
                            .arg(m.cv.precision[std::size_t(k)])));
    QVERIFY2(m.cv.recall[std::size_t(k)] >= 0.9,
             qPrintable(QString("%1 recall[%2]=%3")
                            .arg(int(method))
                            .arg(k)
                            .arg(m.cv.recall[std::size_t(k)])));
  }
  QCOMPARE(m.provenance.value("trainingSetHash").toString().size(),
           qsizetype(64));
  QCOMPARE(m.provenance.value("sampleValuesHash").toString().size(),
           qsizetype(64));
  QCOMPARE(m.provenance.value("cvStandardization").toString(),
           QString("full_sample_transductive"));
  QCOMPARE(m.provenance.value("method").toString(),
           QString(method == Classifier::Lda   ? "lda"
                   : method == Classifier::Qda ? "qda"
                                               : "knn"));
  if (method == Classifier::Knn) {
    QVERIFY(m.model.means.values.empty());
    QCOMPARE(m.model.trainValues.rows(), std::size_t(128));
    QVERIFY(m.model.knnNeighbors >= 1);
  } else {
    QCOMPARE(m.model.means.rows(), std::size_t(2));
    QCOMPARE(m.model.covariances.size(),
             method == Classifier::Lda ? std::size_t(4) : std::size_t(8));
  }
  const Classification a = FaciesTrainingService::classifyWith(s, m, o);
  const Classification b = FaciesTrainingService::classifyWith(s, m, o);
  QVERIFY2(a.ok, qPrintable(a.error));
  QCOMPARE(a.labels, b.labels);
  QCOMPARE(a.confidence, b.confidence);
  QCOMPARE(a.squaredDistance, b.squaredDistance);
  paleo::cluster::Matrix x;
  QString why;
  QVERIFY2(matrixWithModelNormalization(s, m, &x, &why), qPrintable(why));
  const auto trained =
      paleo::cluster::trainSupervised(x, t.labels, supervisedOptionsFor(method));
  QVERIFY2(trained.ok, trained.error.c_str());
  const auto predicted = paleo::cluster::predictSupervised(x, trained.model);
  QVERIFY2(predicted.ok, predicted.error.c_str());
  QCOMPARE(a.labels, trained.labels);
  QCOMPARE(a.labels, predicted.labels);
  for (std::size_t i = 0; i < a.confidence.size(); ++i) {
    QVERIFY(a.confidence[i] == trained.confidence[i]);
    QVERIFY(a.confidence[i] == predicted.confidence[i]);
    // 三族同口径：squaredDistance 也逐位一致（LDA/QDA = 马氏距离，
    // kNN = 所用近邻均方距）。
    QVERIFY(a.squaredDistance[i] == trained.squaredDistance[i]);
    QVERIFY(a.squaredDistance[i] == predicted.squaredDistance[i]);
  }
  QCOMPARE(a.provenance.value("origin").toString(),
           QString("crossplot_supervised"));
  QCOMPARE(a.provenance.value("geologicalMeaning").toString(),
           QString("supervised_named_classes:砂岩,泥岩"));
  QCOMPARE(a.provenance.value("confidence").toString(),
           QString(method == Classifier::Knn ? "winning_class_vote_share"
                                             : "maximum_plug_in_posterior"));
  QCOMPARE(a.provenance.value("normalizationSource").toString(),
           QString("model_normalization_stats"));
  QCOMPARE(a.provenance.value("trainingSetHash"),
           m.provenance.value("trainingSetHash"));
  // 指纹分键：推理层 parameterHash 只覆盖推理参数；训练值保留为
  // trainingParameterHash（不把训练指纹当输入链式再哈希）。
  QCOMPARE(a.provenance.value("trainingParameterHash").toString().size(),
           qsizetype(64));
  QCOMPARE(a.provenance.value("trainingParameterHash"),
           m.provenance.value("parameterHash"));
  QCOMPARE(a.provenance.value("parameterHash").toString().size(),
           qsizetype(64));
  QVERIFY(a.provenance.value("parameterHash") !=
          m.provenance.value("parameterHash"));
  int correct = 0;
  for (std::size_t i = 0; i < s.rows(); ++i)
    correct += a.labels[i] == int(i % 2);
  QVERIFY2(double(correct) / double(s.rows()) >= 0.95,
           qPrintable(QString("method=%1 correct_ratio=%2")
                          .arg(int(method))
                          .arg(double(correct) / double(s.rows()))));
  qint64 total = 0;
  for (auto c : a.counts)
    total += c;
  QCOMPARE(total, qint64(s.rows()));
}
} // namespace
class TestFaciesSupervised : public QObject {
  Q_OBJECT
private slots:
  void trainAndValidate();
  void inferenceConsistency();
  void emptyStates();
  void sparseClassCodesAndOrphanNames();
  void sampleVolumeGates();
  void trainingHashRowBinding();
  void somServiceBranch();
  void gmmChunkProvenance();
  void supervisedRasterChain();
  void maskedLowConfidence();
  void halfSampledCompanions();
  void writerNegativePaths();
  void commitFailureUnderPurgeLease();
  void staleModelGate();
  void normalizationNoDrift();
  void supervisedGate();
};
void TestFaciesSupervised::trainAndValidate() {
  for (Classifier m : {Classifier::Lda, Classifier::Qda, Classifier::Knn})
    trainInferenceOracle(m);
  // 训练后改样本维度 → classifyWith 专拒路径（维度不一致），样本集合法：
  // values 补齐 rows*3（行数先固定，别用随 values 增长的 rows() 做循环上界
  // ——那会造出 191/3 的畸形集，被 validate 先行拒答而走不到专拒），
  // CrossplotSamples::validate 放行后才撞专拒。
  const SampleSet s = twoClassGrid(8, 8, 1, 4, 11);
  const TrainingSet t = twoClassLabels(s);
  ClassificationOptions o;
  o.method = Classifier::Lda;
  TrainedModel m;
  QString error;
  QVERIFY(FaciesTrainingService::train(s, t, o, &m, &error));
  const int rows = int(s.rows());
  SampleSet wide = s;
  wide.names << "C";
  wide.units << "u";
  for (int i = 0; i < rows; ++i)
    wide.values.push_back(0);
  QCOMPARE(int(wide.values.size()), rows * 3);
  QCOMPARE(int(wide.rows()), rows);
  QCOMPARE(wide.names.size(), 3);
  const auto wrong = FaciesTrainingService::classifyWith(wide, m, o);
  QVERIFY(!wrong.ok);
  QVERIFY2(wrong.error.contains("模型维度"), qPrintable(wrong.error));
}
void TestFaciesSupervised::inferenceConsistency() {
  // 逐族确定性 + train 期/推理一致性已在 trainInferenceOracle 覆盖；这里
  // 补 kNN 服务层投票语义：等距平票落较小 classIds 下标、vote share 即
  // confidence、squaredDistance 为所用近邻均方距。
  const SampleSet s = twoClassGrid(4, 4, 1, 4, 5);
  const TrainingSet t = twoClassLabels(s);
  ClassificationOptions o;
  o.method = Classifier::Knn;
  o.knnNeighbors = 2;
  TrainedModel m;
  QString error;
  QVERIFY2(FaciesTrainingService::train(s, t, o, &m, &error), qPrintable(error));
  const auto r = FaciesTrainingService::classifyWith(s, m, o);
  QVERIFY2(r.ok, qPrintable(r.error));
  for (double c : r.confidence) {
    QVERIFY(c >= 0 && c <= 1);
    QVERIFY(c == std::floor(c * 2 + 0.5) / 2); // k=2 票占比为 0.5 的倍数
  }
  // 等距平票：2 训练点夹角中点的查询点到两类距离相等 → 赢较小 classIds
  // 下标（0），票占比 0.5，squaredDistance 为两近邻均方距。
  SampleSet tie;
  tie.names << "A" << "B";
  tie.units << "u" << "u";
  for (auto pt : {std::pair<double, double>{0, 0}, {0, 2}, {2, 0}, {2, 2}}) {
    tie.values.push_back(pt.first);
    tie.values.push_back(pt.second);
    tie.locations << Location{};
  }
  TrainingSet tieLabels;
  tieLabels.labels = {0, 0, 1, 1};
  tieLabels.classNames << "砂岩" << "泥岩";
  tieLabels.provenance.insert("labelProvenance", "free_text_lasso");
  ClassificationOptions tieOptions;
  tieOptions.method = Classifier::Knn;
  tieOptions.knnNeighbors = 2;
  tieOptions.cvFolds = 2;
  TrainedModel tieModel;
  QVERIFY2(FaciesTrainingService::train(tie, tieLabels, tieOptions, &tieModel,
                                        &error),
           qPrintable(error));
  // 查询行把四个训练行复制进来再加一行中点 (1,0)：标注行照常训练，中点行
  // 无标注、与 (0,0)/(2,0) 等距（d²=1,1），另外两点 d²=5。
  SampleSet query = tie;
  query.values.push_back(1);
  query.values.push_back(0);
  query.locations << Location{};
  const auto pr = FaciesTrainingService::classifyWith(query, tieModel, tieOptions);
  QVERIFY2(pr.ok, qPrintable(pr.error));
  QCOMPARE(pr.labels.back(), 0); // 平票 → 较小 classIds 下标
  QCOMPARE(pr.confidence.back(), 0.5);
  QCOMPARE(pr.squaredDistance.back(), 1.0);
}
void TestFaciesSupervised::emptyStates() {
  const SampleSet s = twoClassGrid(8, 8, 1, 4, 11);
  const TrainingSet t = twoClassLabels(s);
  ClassificationOptions o;
  o.method = Classifier::Lda;
  // 零标注 → 拒，error 含「无标注」义
  TrainingSet none = t;
  std::fill(none.labels.begin(), none.labels.end(), -1);
  const auto zero = FaciesTrainingService::validateTrainingSet(s, none, o);
  QVERIFY(!zero.ok);
  QVERIFY(zero.error.contains("无标注"));
  QCOMPARE(zero.labeledCount, 0);
  QCOMPARE(zero.classCount, 0);
  // 单类 → 拒
  TrainingSet single = t;
  for (auto &l : single.labels)
    l = 0;
  const auto one = FaciesTrainingService::validateTrainingSet(s, single, o);
  QVERIFY(!one.ok);
  QVERIFY(one.error.contains("两个"));
  QCOMPARE(one.classCount, 1);
  // 10:1 失衡 → 放行但 warnings 非空
  TrainingSet skewed = t;
  skewed.labels.assign(s.rows(), -1);
  for (std::size_t i = 0; i < s.rows(); ++i)
    if (i % 2 == 0)
      skewed.labels[i] = 0; // 32 个类 0
  for (std::size_t i = 0; i < 3; ++i)
    skewed.labels[2 * i + 1] = 1; // 3 个类 1：32:3 > 10:1 且 <5 样本
  const auto skew =
      FaciesTrainingService::validateTrainingSet(s, skewed, o);
  QVERIFY2(skew.ok, qPrintable(skew.error));
  QVERIFY(!skew.warnings.isEmpty());
  bool sawImbalance = false, sawSmall = false;
  for (const auto &w : skew.warnings) {
    sawImbalance |= w.contains("失衡");
    sawSmall |= w.contains("样本");
  }
  QVERIFY2(sawImbalance, qPrintable(skew.warnings.join("|")));
  QVERIFY2(sawSmall, qPrintable(skew.warnings.join("|")));
  QCOMPARE(skew.classCount, 2);
  QCOMPARE(skew.labeledCount, 35);
  // 空样本集 → 拒
  const SampleSet empty;
  QVERIFY(!FaciesTrainingService::validateTrainingSet(empty, t, o).ok);
  // 标注行数与样本行数不符 → 拒
  TrainingSet mismatched = t;
  mismatched.labels.pop_back();
  QVERIFY(!FaciesTrainingService::validateTrainingSet(s, mismatched, o).ok);
  // 非监督方法 / 参数下限：中文拒答
  ClassificationOptions kmeans = o;
  kmeans.method = Classifier::KMeans;
  QVERIFY(!FaciesTrainingService::validateTrainingSet(s, t, kmeans).ok);
  ClassificationOptions folds = o;
  folds.cvFolds = 1;
  const auto badFolds = FaciesTrainingService::validateTrainingSet(s, t, folds);
  QVERIFY(!badFolds.ok);
  QVERIFY(badFolds.error.contains("折数"));
  ClassificationOptions neighbors = o;
  neighbors.method = Classifier::Knn;
  neighbors.knnNeighbors = 0;
  const auto badK = FaciesTrainingService::validateTrainingSet(s, t, neighbors);
  QVERIFY(!badK.ok);
  QVERIFY(badK.error.contains("近邻"));
  // 类码无类名 / 超出栅格编码范围 → 拒
  TrainingSet orphan = t;
  orphan.labels[0] = 5;
  QVERIFY(!FaciesTrainingService::validateTrainingSet(s, orphan, o).ok);
  TrainingSet huge = t;
  huge.labels[0] = 300;
  QVERIFY(!FaciesTrainingService::validateTrainingSet(s, huge, o).ok);
}
void TestFaciesSupervised::sparseClassCodesAndOrphanNames() {
  // 高 1 复现序列（经 workflow 公开 API）：重复标注覆盖 + 稀疏类码。
  // 旧实现按 classCount 分配 counts 却用原始类码索引 → 越界写 + 类名错配，
  // 把 20v20 均衡集误拒成「类『泥岩』仅有 0 个样本」。
  const SampleSet s = twoClassGrid(2, 20, 1, 4, 13); // 40 行
  PaleoProjectStore store;
  PaleoTaskService tasks(&store);
  FaciesClassifyWorkflow wf(&tasks, &store, nullptr);
  wf.setSamples(std::make_shared<SampleSet>(s));
  QVector<int> first, second;
  for (int i = 0; i < 40; ++i)
    (i < 20 ? first : second) << i;
  wf.assignTrainingLabel(first, "砂岩");   // code 0，随后被覆盖
  wf.assignTrainingLabel(first, "泥岩");   // code 1，覆盖前 20 行
  wf.assignTrainingLabel(second, "灰岩");  // code 2，后 20 行
  const auto &t = wf.trainingSet();
  QCOMPARE(t.classNames, (QStringList{"砂岩", "泥岩", "灰岩"}));
  QCOMPARE(int(t.labels.size()), 40);
  for (int i = 0; i < 20; ++i)
    QCOMPARE(t.labels[std::size_t(i)], 1);
  for (int i = 20; i < 40; ++i)
    QCOMPARE(t.labels[std::size_t(i)], 2);
  ClassificationOptions o;
  o.method = Classifier::Lda;
  o.cvFolds = 2;
  const auto pre = FaciesTrainingService::validateTrainingSet(s, t, o);
  QVERIFY2(pre.ok, qPrintable(pre.error));
  QCOMPARE(pre.classCount, 2);   // 只数有样本的类
  QCOMPARE(pre.labeledCount, 40);
  bool sawOrphan = false;
  for (const auto &w : pre.warnings)
    sawOrphan |= w.contains("砂岩") && w.contains("没有标注样本");
  QVERIFY2(sawOrphan, qPrintable(pre.warnings.join("|")));
  // 摘要只统计有样本的类，孤立类名单列
  const QString summary = wf.trainingSummary();
  QVERIFY2(summary.contains("40"), qPrintable(summary));
  QVERIFY2(summary.contains("2 类"), qPrintable(summary));
  QVERIFY2(summary.contains("未使用的类名：砂岩"), qPrintable(summary));
  QVERIFY2(!summary.contains("砂岩：0"), qPrintable(summary));
  // 训练放行
  TrainedModel m;
  QString error;
  QVERIFY2(FaciesTrainingService::train(s, t, o, &m, &error), qPrintable(error));
  QCOMPARE(m.classNames, (QStringList{"泥岩", "灰岩"}));
  const auto r = FaciesTrainingService::classifyWith(s, m, o);
  QVERIFY2(r.ok, qPrintable(r.error));
  for (int label : r.labels) {
    QVERIFY(label == 1 || label == 2); // 类码回填真实编码，不混入孤立类 0
  }
}
void TestFaciesSupervised::sampleVolumeGates() {
  // 中 1：样本量下限与 supervised.cpp/crossValidate 同口径中文预检。
  const SampleSet s = threeClassGrid(4, 1, 4, 5); // 16 行、3 维
  TrainingSet t = threeClassLabels(s);
  ClassificationOptions o;
  o.method = Classifier::Lda;
  // LDA：标注数 − 类数 < 维度 → 拒（4 标注 − 2 类 = 2 < d=3）
  TrainingSet few = t;
  few.labels.assign(s.rows(), -1);
  few.labels[0] = 0;
  few.labels[1] = 0;
  few.labels[2] = 1;
  few.labels[3] = 1;
  const auto ldaGate = FaciesTrainingService::validateTrainingSet(s, few, o);
  QVERIFY(!ldaGate.ok);
  QVERIFY(ldaGate.error.contains("LDA"));
  // kNN：两类中类 1 只剩 1 个样本 → 撞 cvFloor（每类≥2）拒，不再有 kNN 例外
  TrainingSet single = few;
  single.labels[3] = -1; // 类 1 只剩 1 个样本
  ClassificationOptions knn = o;
  knn.method = Classifier::Knn;
  const auto knnGate = FaciesTrainingService::validateTrainingSet(s, single, knn);
  QVERIFY(!knnGate.ok);
  QVERIFY(knnGate.error.contains("交叉验证要求每类至少"));
  // LDA 同集：先撞每类 2 的下限（自由度检查之前），也是中文
  const auto ldaSingle = FaciesTrainingService::validateTrainingSet(s, single, o);
  QVERIFY(!ldaSingle.ok);
  QVERIFY(!ldaSingle.error.isEmpty());
  // QDA：两类各 2 样本、d=3 → 拒（需 d+1=4）
  TrainingSet pair = t;
  pair.labels.assign(s.rows(), -1);
  pair.labels[0] = 0;
  pair.labels[1] = 0;
  pair.labels[2] = 1;
  pair.labels[3] = 1;
  ClassificationOptions qda = o;
  qda.method = Classifier::Qda;
  const auto qdaGate = FaciesTrainingService::validateTrainingSet(s, pair, qda);
  QVERIFY(!qdaGate.ok);
  QVERIFY(qdaGate.error.contains("QDA"));
  // 每类 ≥2（类 0 两个、类 1 四个）：LDA 自由度 6−2=4 ≥ d=3，可过 validate
  TrainingSet quad = t;
  quad.labels.assign(s.rows(), -1);
  quad.labels[0] = 0;
  quad.labels[1] = 0;
  quad.labels[2] = 1;
  quad.labels[3] = 1;
  quad.labels[4] = 1;
  quad.labels[5] = 1; // 类 0 有 2、类 1 有 4
  QVERIFY(FaciesTrainingService::validateTrainingSet(s, quad, o).ok);
  // 算法层错误不得英文直达：CV 折内 LDA 自由度不足（过 validate 后折内
  // 仍失败）→ train() 统一包中文前缀「训练失败：」。
  TrainingSet foldFail = t;
  foldFail.labels.assign(s.rows(), -1);
  foldFail.labels[0] = 0;
  foldFail.labels[1] = 0; // 类 0：2 个
  for (int i = 2; i < 6; ++i)
    foldFail.labels[i] = 1; // 类 1：4 个（6 标注 − 2 类 = 4 ≥ d=3，过 validate）
  TrainedModel m;
  QString error;
  QVERIFY2(FaciesTrainingService::validateTrainingSet(s, foldFail, o).ok,
           "样本量门禁应放行，留给 CV 折内拒绝");
  QVERIFY(!FaciesTrainingService::train(s, foldFail, o, &m, &error));
  QVERIFY2(error.contains("训练失败"), qPrintable(error));
  // 真·单类（全部同一类码）→ validate 前置中文拒
  TrainingSet degenerate = t;
  degenerate.labels.assign(s.rows(), 0);
  QVERIFY(!FaciesTrainingService::train(s, degenerate, o, &m, &error));
  QVERIFY(!error.contains("Fewer than two labeled classes"));
}
void TestFaciesSupervised::trainingHashRowBinding() {
  const SampleSet s = twoClassGrid(2, 5, 1, 4, 17); // 10 行、2 维
  ClassificationOptions o;
  o.method = Classifier::Lda;
  o.cvFolds = 2;
  auto hashOf = [&](const std::vector<int> &labels,
                    const QStringList &names) {
    TrainingSet t;
    t.labels = labels;
    t.classNames = names;
    t.provenance.insert("labelProvenance", "free_text_lasso");
    TrainedModel m;
    QString error;
    if (!FaciesTrainingService::train(s, t, o, &m, &error))
      return QString("ERR:" + error);
    return m.provenance.value("trainingSetHash").toString();
  };
  // 每类 4 样本：LDA 自由度 8−2=6 ≥ d=2，且 2 折 CV 每折训练半 2/类可过。
  const auto a = hashOf({0, 0, 0, 0, 1, 1, 1, 1, -1, -1},
                        {"砂岩", "泥岩"});
  // 同码序列整体挪一行 → 指纹必须不同（行绑定）
  const auto shifted = hashOf({-1, 0, 0, 0, 0, 1, 1, 1, 1, -1},
                              {"砂岩", "泥岩"});
  // 两类编码互换行位 → 不同
  const auto swapped = hashOf({1, 1, 1, 1, 0, 0, 0, 0, -1, -1},
                              {"砂岩", "泥岩"});
  // 第 1、5 行对调类码 → 不同
  const auto moved = hashOf({0, 1, 1, 1, 1, 0, 0, 0, 0, 0},
                            {"砂岩", "泥岩"});
  // 类名不同 → 不同
  const auto renamed = hashOf({0, 0, 0, 0, 1, 1, 1, 1, -1, -1},
                              {"砂岩", "粉砂"});
  QVERIFY2(!a.startsWith("ERR:"), qPrintable(a));
  QVERIFY(a != shifted);
  QVERIFY(a != swapped);
  QVERIFY(a != moved);
  QVERIFY(a != renamed);
  QCOMPARE(a.size(), 64);
  QCOMPARE(shifted.size(), 64);
  // 同输入 → 同指纹（确定性）
  QCOMPARE(a, hashOf({0, 0, 0, 0, 1, 1, 1, 1, -1, -1},
                     {"砂岩", "泥岩"}));
}
void TestFaciesSupervised::somServiceBranch() {
  const SampleSet s = twoClassGrid(8, 8, 1, 4, 11);
  ClassificationOptions o;
  o.method = Classifier::Som;
  o.somWidth = 3;
  o.somHeight = 2;
  o.maxIterations = 20;
  const auto r = FaciesClassificationService::classify(s, o);
  QVERIFY2(r.ok, qPrintable(r.error));
  QCOMPARE(r.labels.size(), s.rows());
  QCOMPARE(r.confidence.size(), s.rows());
  QVERIFY(r.provenance.contains("somWidth"));
  QVERIFY(r.provenance.contains("somHeight"));
  QCOMPARE(r.provenance.value("somWidth").toInt(), 3);
  QCOMPARE(r.provenance.value("somHeight").toInt(), 2);
  QCOMPARE(r.provenance.value("somGrid").toInt(), 6);
  QCOMPARE(r.provenance.value("epochs").toInt(), 20);
  QCOMPARE(r.provenance.value("origin").toString(),
           QString("crossplot_som"));
  for (int label : r.labels)
    QVERIFY(label >= 0 && label < 6);
  qint64 total = 0;
  for (auto c : r.counts)
    total += c;
  QCOMPARE(total, qint64(s.rows()));
  // 网格超出分类栅格编码范围 → 拒
  ClassificationOptions bad = o;
  bad.somWidth = 20;
  bad.somHeight = 20;
  const auto rejected = FaciesClassificationService::classify(s, bad);
  QVERIFY(!rejected.ok);
  QVERIFY(!rejected.error.isEmpty());
}
void TestFaciesSupervised::gmmChunkProvenance() {
  const SampleSet s = twoClassGrid(8, 8, 1, 4, 11);
  ClassificationOptions o;
  o.method = Classifier::Gmm;
  o.k = 2;
  o.emChunkBudgetBytes = 512; // 64×2×8 B = 1 KiB > 512 → 必走分块 EM
  const auto r = FaciesClassificationService::classify(s, o);
  QVERIFY2(r.ok, qPrintable(r.error));
  QVERIFY(r.provenance.contains("chunkedEm"));
  QVERIFY2(r.provenance.value("chunkedEm").toBool(), "小预算必须走分块 EM");
  QCOMPARE(r.provenance.value("emChunkBudgetBytes").toLongLong(), 512);
  qint64 total = 0;
  for (auto c : r.counts)
    total += c;
  QCOMPARE(total, qint64(s.rows()));
}
void TestFaciesSupervised::supervisedRasterChain() {
  Rig rig;
  QVERIFY(rig.cat.open(rig.dir.path()));
  QVERIFY(rig.project.createProject(rig.dir.filePath("test.qgz")));
  QVERIFY(rig.manifest.open());
  auto samples = std::make_shared<SampleSet>(twoClassGrid(16, 8, 1, 4, 11));
  FaciesClassifyWorkflow wf(&rig.tasks, &rig.store, &rig.layers);
  wf.setCatalog(&rig.cat, rig.dir.path());
  wf.setSamples(samples);
  QVector<int> sand, shale;
  for (int i = 0; i < int(samples->rows()); ++i)
    (i % 2 ? shale : sand) << i;
  wf.assignTrainingLabel(sand, "砂岩");
  wf.assignTrainingLabel(shale, "泥岩");
  QCOMPARE(wf.trainingSet().classNames, (QStringList{"砂岩", "泥岩"}));
  QCOMPARE(int(wf.trainingSet().labels.size()), 128);
  QCOMPARE(wf.trainingSet().provenance.value("labelProvenance").toString(),
           QString("free_text_lasso"));
  QVERIFY(wf.trainingSummary().contains("128"));
  ClassificationOptions o;
  o.method = Classifier::Lda;
  QSignalSpy trained(&wf, &FaciesClassifyWorkflow::trainingReady);
  QSignalSpy trainFailed(&wf, &FaciesClassifyWorkflow::failed);
  wf.train(o);
  QVERIFY2(trained.wait(10000), qPrintable(trainFailed.isEmpty()
                                               ? QString()
                                               : trainFailed.first()
                                                     .first()
                                                     .toString()));
  const auto report = trained.first().first().toMap();
  // provenance/report 键类型口径（批 3 UI 消费面）：method = 字符串族名；
  // supervisedMethod = int(Classifier)（5/6/7 = Lda/Qda/Knn）。
  QCOMPARE(report.value("method").toString(), QString("lda"));
  QCOMPARE(report.value("supervisedMethod").toInt(), int(Classifier::Lda));
  QCOMPARE(report.value("classNames").toStringList(),
           (QStringList{"砂岩", "泥岩"}));
  QCOMPARE(report.value("folds").toInt(), 5);
  QCOMPARE(report.value("classIds").toList().size(), 2);
  QCOMPARE(report.value("confusionCells").toList().size(),
           qsizetype(4)); // 2×2 平铺
  QCOMPARE(report.value("labeledCount").toInt(), 128);
  for (const auto &v : report.value("precision").toList())
    QVERIFY2(v.toDouble() >= 0.9, qPrintable(QString::number(v.toDouble())));
  for (const auto &v : report.value("recall").toList())
    QVERIFY2(v.toDouble() >= 0.9, qPrintable(QString::number(v.toDouble())));
  // 未训练的方法组合法调用被拒
  ClassificationOptions knn = o;
  knn.method = Classifier::Knn;
  QVERIFY(!wf.classify(knn));
  auto *task = wf.classify(o);
  QVERIFY(task);
  QSignalSpy finished(task, &PaleoTask::finished);
  if (!task->isFinished())
    QVERIFY(finished.wait(10000));
  QVERIFY2(wf.classification().ok, qPrintable(wf.classification().error));
  QCOMPARE(wf.classification().provenance.value("origin").toString(),
           QString("crossplot_supervised"));
  const auto product = wf.write("D53");
  QVERIFY2(product.ok, qPrintable(product.error));
  QVERIFY(QFile::exists(product.path));
  QCOMPARE(product.confidenceLayerId, QString("confidence.D53.crossplot"));
  QCOMPARE(product.maskedLayerId,
           QString("predict.D53.crossplot.masked"));
  QVERIFY(!product.maskedAssetId.isEmpty());
  QVERIFY(!product.maskedVersionId.isEmpty());
  QVERIFY(QFile::exists(product.confidencePath));
  QVERIFY(QFile::exists(product.maskedPath));
  // catalog 三件 DERIVED 版本，父版本一致、训练指纹入库、extra 带 layer_id
  const auto main = derivedVersion(rig.cat, "facies_classification");
  const auto conf = derivedVersion(rig.cat, "confidence");
  const auto masked = derivedVersion(rig.cat, "facies_classification_masked");
  QVERIFY(!main.id.isEmpty());
  QVERIFY(!conf.id.isEmpty());
  QVERIFY(!masked.id.isEmpty());
  QCOMPARE(main.stage, QString("DERIVED"));
  QCOMPARE(conf.stage, QString("DERIVED"));
  QCOMPARE(masked.stage, QString("DERIVED"));
  QCOMPARE(main.parentVersionIds, QStringList({"raw-a", "raw-b"}));
  QCOMPARE(conf.parentVersionIds, main.parentVersionIds);
  QCOMPARE(masked.parentVersionIds, main.parentVersionIds);
  QCOMPARE(conf.extra.value("trainingSetHash").toString().size(),
           qsizetype(64));
  QCOMPARE(masked.extra.value("trainingSetHash").toString().size(),
           qsizetype(64));
  QCOMPARE(main.extra.value("layer_id").toString(),
           QString("predict.D53.crossplot"));
  QCOMPARE(conf.extra.value("layer_id").toString(),
           QString("confidence.D53.crossplot"));
  QCOMPARE(masked.extra.value("layer_id").toString(),
           QString("predict.D53.crossplot.masked"));
  QCOMPARE(masked.extra.value("confidenceMaskThreshold").toDouble(), 0.5);
  QVERIFY(!conf.sha256.isEmpty());
  QVERIFY(!masked.sha256.isEmpty());
  // 三个文件同网格几何；置信度 Float32 nodata=-9999 且逐位等于分类 confidence
  std::unique_ptr<void, decltype(&GDALClose)> dsMain(
      GDALOpen(product.path.toUtf8().constData(), GA_ReadOnly), GDALClose);
  std::unique_ptr<void, decltype(&GDALClose)> dsConf(
      GDALOpen(product.confidencePath.toUtf8().constData(), GA_ReadOnly),
      GDALClose);
  std::unique_ptr<void, decltype(&GDALClose)> dsMasked(
      GDALOpen(product.maskedPath.toUtf8().constData(), GA_ReadOnly),
      GDALClose);
  QVERIFY(dsMain && dsConf && dsMasked);
  for (auto *ds : {dsMain.get(), dsConf.get(), dsMasked.get()}) {
    QCOMPARE(GDALGetRasterYSize(ds), 16);
    QCOMPARE(GDALGetRasterXSize(ds), 8);
    double gt[6];
    QCOMPARE(GDALGetGeoTransform(ds, gt), CE_None);
    for (int i = 0; i < 6; ++i)
      QCOMPARE(gt[i], samples->grid.transform[std::size_t(i)]);
  }
  auto *confBand = GDALGetRasterBand(dsConf.get(), 1);
  QCOMPARE(GDALGetRasterDataType(confBand), GDT_Float32);
  int hasNo = 0;
  const double nodata = GDALGetRasterNoDataValue(confBand, &hasNo);
  QVERIFY(hasNo);
  QCOMPARE(nodata, -9999);
  std::vector<float> pixels(128);
  QCOMPARE(GDALRasterIO(confBand, GF_Read, 0, 0, 8, 16, pixels.data(), 8, 16,
                        GDT_Float32, 0, 0),
           CE_None);
  for (int i = 0; i < 128; ++i) {
    QVERIFY(pixels[std::size_t(i)] != -9999.0f); // 全采样
    QCOMPARE(pixels[std::size_t(i)],
             float(wf.classification().confidence[std::size_t(i)]));
    QVERIFY(pixels[std::size_t(i)] >= 0 && pixels[std::size_t(i)] <= 1);
  }
  // 中 5：metadata key 读回断言——置信度件 PALEO_PROVENANCE JSON 含训练指纹；
  // 掩膜件与主图 PALEO_CLASSIFICATION 串相等。
  const char *confMeta =
      GDALGetMetadataItem(dsConf.get(), "PALEO_PROVENANCE", nullptr);
  QVERIFY(confMeta && *confMeta);
  const auto confJson = QJsonDocument::fromJson(QByteArray(confMeta)).object();
  QCOMPARE(confJson.value("trainingSetHash").toString().size(), qsizetype(64));
  QCOMPARE(confJson.value("origin").toString(),
           QString("crossplot_supervised"));
  const char *mainMeta =
      GDALGetMetadataItem(dsMain.get(), "PALEO_CLASSIFICATION", nullptr);
  const char *maskedMeta =
      GDALGetMetadataItem(dsMasked.get(), "PALEO_CLASSIFICATION", nullptr);
  QVERIFY(mainMeta && *mainMeta);
  QVERIFY(maskedMeta && *maskedMeta);
  QCOMPARE(QString(maskedMeta), QString(mainMeta));
  // 掩膜位点 = 低置信位点，且不掩盖主图
  auto *maskedBand = GDALGetRasterBand(dsMasked.get(), 1);
  auto *mainBand = GDALGetRasterBand(dsMain.get(), 1);
  std::vector<unsigned char> maskedPixels(128), mainPixels(128);
  QCOMPARE(GDALRasterIO(maskedBand, GF_Read, 0, 0, 8, 16, maskedPixels.data(),
                        8, 16, GDT_Byte, 0, 0),
           CE_None);
  QCOMPARE(GDALRasterIO(mainBand, GF_Read, 0, 0, 8, 16, mainPixels.data(), 8,
                        16, GDT_Byte, 0, 0),
           CE_None);
  for (int i = 0; i < 128; ++i) {
    const bool expectedLow =
        wf.classification().confidence[std::size_t(i)] < 0.5; // double 口径
    QCOMPARE(maskedPixels[std::size_t(i)] == 255, expectedLow);
    if (maskedPixels[std::size_t(i)] == 255)
      QVERIFY2(mainPixels[std::size_t(i)] != 255,
               qPrintable(QString("pixel %1 masked 但主图 nodata").arg(i)));
  }
  // 换样本清训练集（防行错位）
  wf.setSamples(std::make_shared<SampleSet>(twoClassGrid(4, 4, 1, 4, 3)));
  QVERIFY(wf.trainingSet().labels.empty());
  QVERIFY(wf.trainingSet().classNames.isEmpty());
  QVERIFY(wf.trainingSummary().contains("未标注"));
  QVERIFY(!wf.classify(o)); // 模型已随样本清空 → 先训练
}
void TestFaciesSupervised::maskedLowConfidence() {
  Rig rig;
  QVERIFY(rig.cat.open(rig.dir.path()));
  QVERIFY(rig.project.createProject(rig.dir.filePath("test.qgz")));
  QVERIFY(rig.manifest.open());
  auto samples =
      std::make_shared<SampleSet>(threeClassGrid(9, 1, 0.8, 41)); // 1σ 级交叠
  FaciesClassifyWorkflow wf(&rig.tasks, &rig.store, &rig.layers);
  wf.setCatalog(&rig.cat, rig.dir.path());
  wf.setSamples(samples);
  const TrainingSet t = threeClassLabels(*samples);
  for (int c = 0; c < 3; ++c) {
    QVector<int> rows;
    for (std::size_t i = 0; i < samples->rows(); ++i)
      if (t.labels[i] == c)
        rows << int(i);
    wf.assignTrainingLabel(rows, t.classNames[c]);
  }
  QCOMPARE(wf.trainingSet().classNames,
           (QStringList{"砂岩", "泥岩", "灰岩"}));
  ClassificationOptions o;
  o.method = Classifier::Lda;
  QSignalSpy trained(&wf, &FaciesClassifyWorkflow::trainingReady);
  QSignalSpy trainFailed(&wf, &FaciesClassifyWorkflow::failed);
  wf.train(o);
  QVERIFY2(trained.wait(10000), qPrintable(trainFailed.isEmpty()
                                               ? QString()
                                               : trainFailed.first()
                                                     .first()
                                                     .toString()));
  auto *task = wf.classify(o);
  QVERIFY(task);
  QSignalSpy finished(task, &PaleoTask::finished);
  if (!task->isFinished())
    QVERIFY(finished.wait(10000));
  QVERIFY2(wf.classification().ok, qPrintable(wf.classification().error));
  const double threshold = o.confidenceMaskThreshold;
  QCOMPARE(threshold, 0.5);
  const double used = wf.classification()
                          .provenance.value("confidenceMaskThreshold")
                          .toDouble();
  QCOMPARE(used, 0.5);
  const auto product = wf.write("D53");
  QVERIFY2(product.ok, qPrintable(product.error));
  std::unique_ptr<void, decltype(&GDALClose)> dsConf(
      GDALOpen(product.confidencePath.toUtf8().constData(), GA_ReadOnly),
      GDALClose);
  std::unique_ptr<void, decltype(&GDALClose)> dsMasked(
      GDALOpen(product.maskedPath.toUtf8().constData(), GA_ReadOnly),
      GDALClose);
  std::unique_ptr<void, decltype(&GDALClose)> dsMain(
      GDALOpen(product.path.toUtf8().constData(), GA_ReadOnly), GDALClose);
  QVERIFY(dsConf && dsMasked && dsMain);
  const int rows = 9, cols = 9;
  std::vector<float> confPixels(std::size_t(rows * cols));
  std::vector<unsigned char> maskedPixels(std::size_t(rows * cols)),
      mainPixels(std::size_t(rows * cols));
  QCOMPARE(GDALRasterIO(GDALGetRasterBand(dsConf.get(), 1), GF_Read, 0, 0,
                        cols, rows, confPixels.data(), cols, rows,
                        GDT_Float32, 0, 0),
           CE_None);
  QCOMPARE(GDALRasterIO(GDALGetRasterBand(dsMasked.get(), 1), GF_Read, 0, 0,
                        cols, rows, maskedPixels.data(), cols, rows,
                        GDT_Byte, 0, 0),
           CE_None);
  QCOMPARE(GDALRasterIO(GDALGetRasterBand(dsMain.get(), 1), GF_Read, 0, 0,
                        cols, rows, mainPixels.data(), cols, rows, GDT_Byte,
                        0, 0),
           CE_None);
  int maskedCount = 0, lowConfidence = 0;
  for (int i = 0; i < rows * cols; ++i) {
    // 低 9：掩膜判定用内存 double；Float32 落盘有 24 位尾数截断，逐点比照
    // 与内存口径（classification().confidence）对齐，截断差给一位浮点容差。
    const double inMemory = wf.classification().confidence[std::size_t(i)];
    QVERIFY(std::abs(confPixels[std::size_t(i)] - inMemory) < 1e-6);
    const bool low = inMemory < used;
    if (low)
      ++lowConfidence;
    QCOMPARE(maskedPixels[std::size_t(i)] == 255, low);
    if (maskedPixels[std::size_t(i)] == 255) {
      ++maskedCount;
      // 掩膜不掩盖主图：同位置分类图非 nodata
      QVERIFY2(mainPixels[std::size_t(i)] != 255,
               qPrintable(QString("pixel %1 掩膜位主图 nodata").arg(i)));
      QVERIFY(mainPixels[std::size_t(i)] < 3);
    } else {
      QCOMPARE(int(maskedPixels[std::size_t(i)]),
               int(mainPixels[std::size_t(i)]));
    }
  }
  QVERIFY2(maskedCount > 0 && maskedCount < rows * cols,
           qPrintable(QString("maskedCount=%1 lowConfidence=%2")
                          .arg(maskedCount)
                          .arg(lowConfidence)));
  QCOMPARE(maskedCount, lowConfidence);
}
void TestFaciesSupervised::halfSampledCompanions() {
  // 中 6/Low-CRS：半采样夹具——未采样像元走 nodata 分支；三图同 CRS。
  Rig rig;
  QVERIFY(rig.cat.open(rig.dir.path()));
  QVERIFY(rig.project.createProject(rig.dir.filePath("test.qgz")));
  QVERIFY(rig.manifest.open());
  const QString crs = DataCatalog::localGridCrsWkt();
  QVERIFY(!crs.isEmpty());
  // 只采 {0,1,2,3,12,13,14,15} 八个像元（半数）：行 0-3（上排）= 类 0；
  // 行 4-7（下排）= 类 1。每类 4 样本，2 折 CV 每折训练半 2/类可过 LDA。
  auto samples = std::make_shared<SampleSet>(halfSampledGrid(
      {0, 1, 2, 3, 12, 13, 14, 15},
      {{-3, -3}, {3, -3}, {-3, -3}, {3, -3},
       {-3, 3}, {3, 3}, {-3, 3}, {3, 3}},
      crs));
  FaciesClassifyWorkflow wf(&rig.tasks, &rig.store, &rig.layers);
  wf.setCatalog(&rig.cat, rig.dir.path());
  wf.setSamples(samples);
  wf.assignTrainingLabel({0, 1, 2, 3}, "砂岩");
  wf.assignTrainingLabel({4, 5, 6, 7}, "泥岩");
  ClassificationOptions o;
  o.method = Classifier::Lda;
  o.cvFolds = 2; // 最小类 2 → 折数夹取为 2
  QSignalSpy trained(&wf, &FaciesClassifyWorkflow::trainingReady);
  QSignalSpy trainFailed(&wf, &FaciesClassifyWorkflow::failed);
  wf.train(o);
  QVERIFY2(trained.wait(10000), qPrintable(trainFailed.isEmpty()
                                               ? QString()
                                               : trainFailed.first()
                                                     .first()
                                                     .toString()));
  auto *task = wf.classify(o);
  QVERIFY(task);
  QSignalSpy finished(task, &PaleoTask::finished);
  if (!task->isFinished())
    QVERIFY(finished.wait(10000));
  QVERIFY2(wf.classification().ok, qPrintable(wf.classification().error));
  const auto product = wf.write("D53");
  QVERIFY2(product.ok, qPrintable(product.error));
  std::unique_ptr<void, decltype(&GDALClose)> dsMain(
      GDALOpen(product.path.toUtf8().constData(), GA_ReadOnly), GDALClose);
  std::unique_ptr<void, decltype(&GDALClose)> dsConf(
      GDALOpen(product.confidencePath.toUtf8().constData(), GA_ReadOnly),
      GDALClose);
  std::unique_ptr<void, decltype(&GDALClose)> dsMasked(
      GDALOpen(product.maskedPath.toUtf8().constData(), GA_ReadOnly),
      GDALClose);
  QVERIFY(dsMain && dsConf && dsMasked);
  // 同 CRS（非空 WKT），三图一致
  QString projection;
  for (auto *ds : {dsMain.get(), dsConf.get(), dsMasked.get()}) {
    QCOMPARE(GDALGetRasterYSize(ds), 4);
    QCOMPARE(GDALGetRasterXSize(ds), 4);
    const char *wkt = GDALGetProjectionRef(ds);
    QVERIFY(wkt && *wkt);
    if (projection.isEmpty())
      projection = QString::fromUtf8(wkt);
    else
      QCOMPARE(QString::fromUtf8(wkt), projection);
  }
  std::vector<float> conf(16);
  std::vector<unsigned char> mainPixels(16), maskedPixels(16);
  QCOMPARE(GDALRasterIO(GDALGetRasterBand(dsConf.get(), 1), GF_Read, 0, 0, 4,
                        4, conf.data(), 4, 4, GDT_Float32, 0, 0),
           CE_None);
  QCOMPARE(GDALRasterIO(GDALGetRasterBand(dsMain.get(), 1), GF_Read, 0, 0, 4,
                        4, mainPixels.data(), 4, 4, GDT_Byte, 0, 0),
           CE_None);
  QCOMPARE(GDALRasterIO(GDALGetRasterBand(dsMasked.get(), 1), GF_Read, 0, 0,
                        4, 4, maskedPixels.data(), 4, 4, GDT_Byte, 0, 0),
           CE_None);
  const QSet<int> sampled{0, 1, 2, 3, 12, 13, 14, 15};
  for (int i = 0; i < 16; ++i) {
    if (sampled.contains(i)) {
      QVERIFY(conf[std::size_t(i)] != -9999.0f);
      QVERIFY(conf[std::size_t(i)] >= 0 && conf[std::size_t(i)] <= 1);
      QVERIFY(mainPixels[std::size_t(i)] != 255);
      QVERIFY(maskedPixels[std::size_t(i)] != 255);
    } else {
      QCOMPARE(conf[std::size_t(i)], -9999.0f); // 未采样 → nodata
      QCOMPARE(mainPixels[std::size_t(i)], static_cast<unsigned char>(255));
      QCOMPARE(maskedPixels[std::size_t(i)], static_cast<unsigned char>(255));
    }
  }
}
void TestFaciesSupervised::writerNegativePaths() {
  // 中 4：写入器负路径——畸形 SampleSet / 阈值越界一律中文拒答。
  const SampleSet good = twoClassGrid(4, 4, 1, 4, 11); // 16 行
  const TrainingSet t = twoClassLabels(good);
  ClassificationOptions o;
  o.method = Classifier::Lda;
  o.cvFolds = 2;
  TrainedModel m;
  QString error;
  QVERIFY(FaciesTrainingService::train(good, t, o, &m, &error));
  const auto r = FaciesTrainingService::classifyWith(good, m, o);
  QVERIFY2(r.ok, qPrintable(r.error));
  PaleoProjectStore store;
  QTemporaryDir dir;
  auto guarded = [&](const QString &path, const SampleSet &s,
                     const Classification &cls, double threshold,
                     bool masked) {
    return store.enqueueWrite([&] {
      QString why;
      const bool ok =
          masked ? FaciesClassificationService::writeMaskedRaster(
                       path, s, cls, threshold, &why)
                 : FaciesClassificationService::writeConfidenceRaster(
                       path, s, cls, &why);
      return PaleoProjectStore::WriteResult{ok, why};
    });
  };
  // 非栅格样本（无 grid）→ 几何无效
  SampleSet noGrid = good;
  noGrid.grid = Grid{};
  QVERIFY(!guarded(dir.filePath("a.tif"), noGrid, r, 0.5, false).ok);
  QVERIFY(!guarded(dir.filePath("b.tif"), noGrid, r, 0.5, true).ok);
  // 网格尺寸与样本像元不符 → 映射无效
  SampleSet shrink = good;
  shrink.grid.rows = 3;
  shrink.grid.cols = 3;
  QVERIFY(!guarded(dir.filePath("c.tif"), shrink, r, 0.5, false).ok);
  // 分类与样本不一致（labels 行数不符）→ 几何无效
  Classification bad = r;
  bad.labels.pop_back();
  QVERIFY(!guarded(dir.filePath("d.tif"), good, bad, 0.5, false).ok);
  // 阈值越界 / NaN → 中文拒答
  auto high = guarded(dir.filePath("e.tif"), good, r, 1.5, true);
  QVERIFY(!high.ok);
  QVERIFY(high.error.contains("[0,1]"));
  const double nan = std::numeric_limits<double>::quiet_NaN();
  auto nanRun = guarded(dir.filePath("f.tif"), good, r, nan, true);
  QVERIFY(!nanRun.ok);
  QVERIFY(nanRun.error.contains("[0,1]"));
  // 写队列外直接调 → 队列门禁
  QString why;
  QVERIFY(!FaciesClassificationService::writeConfidenceRaster(
      dir.filePath("g.tif"), good, r, &why));
  QVERIFY(why.contains("写队列"));
  // workflow 级：主图成功、掩膜失败（NaN 阈值）→ 整体失败且报「掩膜」，
  // catalog 不留半套孤儿。commit 阶段失败的负路径见
  // commitFailureUnderPurgeLease（warm catalog + CatalogPurgeLease）。
  Rig rig;
  QVERIFY(rig.cat.open(rig.dir.path()));
  QVERIFY(rig.project.createProject(rig.dir.filePath("test.qgz")));
  QVERIFY(rig.manifest.open());
  auto samples = std::make_shared<SampleSet>(twoClassGrid(8, 8, 1, 4, 11));
  FaciesClassifyWorkflow wf(&rig.tasks, &rig.store, &rig.layers);
  wf.setCatalog(&rig.cat, rig.dir.path());
  wf.setSamples(samples);
  const TrainingSet labels = twoClassLabels(*samples);
  wf.assignTrainingLabel({0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22}, "砂岩");
  QVector<int> shale;
  for (int i = 1; i < int(samples->rows()); i += 2)
    shale << i;
  wf.assignTrainingLabel(shale, "泥岩");
  QCOMPARE(labels.classNames, wf.trainingSet().classNames);
  ClassificationOptions nanOptions = o;
  nanOptions.confidenceMaskThreshold = nan;
  QSignalSpy trained(&wf, &FaciesClassifyWorkflow::trainingReady);
  QSignalSpy trainFailed(&wf, &FaciesClassifyWorkflow::failed);
  wf.train(nanOptions);
  QVERIFY2(trained.wait(10000), qPrintable(trainFailed.isEmpty()
                                               ? QString()
                                               : trainFailed.first()
                                                     .first()
                                                     .toString()));
  auto *task = wf.classify(nanOptions);
  QVERIFY(task);
  QSignalSpy finished(task, &PaleoTask::finished);
  if (!task->isFinished())
    QVERIFY(finished.wait(10000));
  QVERIFY2(wf.classification().ok, qPrintable(wf.classification().error));
  QSignalSpy failed(&wf, &FaciesClassifyWorkflow::failed);
  const auto product = wf.write("D53");
  QVERIFY(!product.ok);
  QVERIFY2(product.error.contains("掩膜"), qPrintable(product.error));
  QCOMPARE(failed.count(), 1);
  QVERIFY(product.path.isEmpty());
  QVERIFY(!QFile::exists(product.confidencePath));
  QVERIFY(!QFile::exists(product.maskedPath));
  QVERIFY(derivedVersion(rig.cat, "facies_classification").id.isEmpty());
  QVERIFY(derivedVersion(rig.cat, "confidence").id.isEmpty());
  QVERIFY(derivedVersion(rig.cat, "facies_classification_masked").id.isEmpty());
}
void TestFaciesSupervised::commitFailureUnderPurgeLease() {
  // 中 1：commit 失败负路径——warm catalog 下 stage() 全程无 mutator（资产
  // 已存在则跳过 addAsset），持 CatalogPurgeLease 后 DataCatalog::addVersion
  // 的 checkWriteThread 拒写 → 主件登记失败分支真被触发。
  Rig rig;
  QVERIFY(rig.cat.open(rig.dir.path()));
  QVERIFY(rig.project.createProject(rig.dir.filePath("test.qgz")));
  QVERIFY(rig.manifest.open());
  auto samples = std::make_shared<SampleSet>(twoClassGrid(8, 8, 1, 4, 11));
  FaciesClassifyWorkflow wf(&rig.tasks, &rig.store, &rig.layers);
  wf.setCatalog(&rig.cat, rig.dir.path());
  wf.setSamples(samples);
  QVector<int> sand, shale;
  for (int i = 0; i < int(samples->rows()); ++i)
    (i % 2 ? shale : sand) << i;
  wf.assignTrainingLabel(sand, "砂岩");
  wf.assignTrainingLabel(shale, "泥岩");
  ClassificationOptions o;
  o.method = Classifier::Lda;
  QSignalSpy trained(&wf, &FaciesClassifyWorkflow::trainingReady);
  QSignalSpy trainFailed(&wf, &FaciesClassifyWorkflow::failed);
  wf.train(o);
  QVERIFY2(trained.wait(10000), qPrintable(trainFailed.isEmpty()
                                               ? QString()
                                               : trainFailed.first()
                                                     .first()
                                                     .toString()));
  auto *task = wf.classify(o);
  QVERIFY(task);
  QSignalSpy finished(task, &PaleoTask::finished);
  if (!task->isFinished())
    QVERIFY(finished.wait(10000));
  QVERIFY2(wf.classification().ok, qPrintable(wf.classification().error));
  // 第一次写入把三个资产烤暖（commit 后资产已在 catalog，再 stage 不再触发
  // addAsset mutator，持锁只卡 addVersion）。
  const auto first = wf.write("D53");
  QVERIFY2(first.ok, qPrintable(first.error));
  const int versions = rig.cat.versions().size();
  const int files = countFiles(rig.dir.filePath("artifacts/derived"));
  QCOMPARE(versions, 3);
  QVERIFY(files >= 3);
  auto lease = CatalogPurgeLease::acquire(rig.cat.catalogPath());
  QVERIFY(lease);
  QVERIFY(CatalogPurgeLease::isHeld(rig.cat.catalogPath()));
  QSignalSpy failed(&wf, &FaciesClassifyWorkflow::failed);
  const auto second = wf.write("D53");
  QVERIFY(!second.ok);
  QVERIFY2(second.error.contains("登记失败"), qPrintable(second.error));
  QCOMPARE(failed.count(), 1);
  // staged 文件被清、catalog 版本数不变——不在库内留半套孤儿
  QCOMPARE(rig.cat.versions().size(), versions);
  QCOMPARE(countFiles(rig.dir.filePath("artifacts/derived")), files);
  QVERIFY(second.path.isEmpty());
  lease.reset();
  // 放锁后重试本层位写入：成功并按资产递增下一版本（文案里的重试路径为实）
  const auto third = wf.write("D53");
  QVERIFY2(third.ok, qPrintable(third.error));
  QCOMPARE(rig.cat.versions().size(), versions + 3);
}
void TestFaciesSupervised::staleModelGate() {
  // 中 2：陈旧模型 hazard——train 后再改标注，classify 必须要求重训，而不是
  // 静默拿旧模型推理。顺带覆盖 clearTraining 作废在途训练任务。
  Rig rig;
  QVERIFY(rig.cat.open(rig.dir.path()));
  QVERIFY(rig.project.createProject(rig.dir.filePath("test.qgz")));
  QVERIFY(rig.manifest.open());
  rig.tasks.setMaxWorkerThreads(1);
  auto samples = std::make_shared<SampleSet>(twoClassGrid(8, 8, 1, 4, 11));
  FaciesClassifyWorkflow wf(&rig.tasks, &rig.store, &rig.layers);
  wf.setCatalog(&rig.cat, rig.dir.path());
  wf.setSamples(samples);
  QVector<int> sand, shale;
  for (int i = 0; i < int(samples->rows()); ++i)
    (i % 2 ? shale : sand) << i;
  wf.assignTrainingLabel(sand, "砂岩");
  wf.assignTrainingLabel(shale, "泥岩");
  ClassificationOptions o;
  o.method = Classifier::Lda;
  QSignalSpy trained(&wf, &FaciesClassifyWorkflow::trainingReady);
  QSignalSpy trainFailed(&wf, &FaciesClassifyWorkflow::failed);
  wf.train(o);
  QVERIFY2(trained.wait(10000), qPrintable(trainFailed.isEmpty()
                                               ? QString()
                                               : trainFailed.first()
                                                     .first()
                                                     .toString()));
  // 追加标注（把一行泥岩改标砂岩）→ 模型变陈旧
  wf.assignTrainingLabel({1}, "砂岩");
  QSignalSpy failed(&wf, &FaciesClassifyWorkflow::failed);
  QVERIFY(!wf.classify(o));
  QCOMPARE(failed.count(), 1);
  QVERIFY2(failed.first().first().toString().contains("重新训练"),
           qPrintable(failed.first().first().toString()));
  // 重新训练后门禁放行
  trained.clear();
  wf.train(o);
  QVERIFY2(trained.wait(10000), qPrintable(trainFailed.isEmpty()
                                               ? QString()
                                               : trainFailed.first()
                                                     .first()
                                                     .toString()));
  auto *task = wf.classify(o);
  QVERIFY(task);
  QSignalSpy finished(task, &PaleoTask::finished);
  if (!task->isFinished())
    QVERIFY(finished.wait(10000));
  QVERIFY2(wf.classification().ok, qPrintable(wf.classification().error));
  // clearTraining 作废在途训练任务：占住唯一工作线程，train 排队无法完成；
  // 此时清训练集 → 任务完成回调被 generation 作废，模型不得复活。
  auto *blocker = rig.tasks.start("block", [](PaleoTask *t) {
    while (!t->cancelRequested())
      QThread::msleep(1);
    return QString();
  });
  QVERIFY(blocker);
  trained.clear();
  wf.train(o);
  wf.clearTraining();
  blocker->requestCancel();
  QTRY_COMPARE(trained.count(), 0);
  QVERIFY(wf.trainingSet().labels.empty());
  QSignalSpy failed2(&wf, &FaciesClassifyWorkflow::failed);
  QVERIFY(!wf.classify(o)); // 模型未复活
  QCOMPARE(failed2.count(), 1);
  QVERIFY2(failed2.first().first().toString().contains("尚未训练"),
           qPrintable(failed2.first().first().toString()));
}
void TestFaciesSupervised::normalizationNoDrift() {
  // 高 2：训练 standardize=false 时，推理严禁按当前样本重估标准化——
  // o.standardize=true/false 两次 classifyWith 必须逐位一致，且 provenance
  // 记实际口径。
  const SampleSet s = twoClassGrid(8, 8, 1, 4, 11);
  const TrainingSet t = twoClassLabels(s);
  ClassificationOptions raw;
  raw.method = Classifier::Lda;
  raw.standardize = false;
  raw.cvFolds = 2;
  TrainedModel m;
  QString error;
  QVERIFY2(FaciesTrainingService::train(s, t, raw, &m, &error),
           qPrintable(error));
  QVERIFY(!m.provenance.contains("normalizationMean"));
  ClassificationOptions withStandardize = raw;
  withStandardize.standardize = true;
  const auto a = FaciesTrainingService::classifyWith(s, m, withStandardize);
  const auto b = FaciesTrainingService::classifyWith(s, m, raw);
  QVERIFY2(a.ok, qPrintable(a.error));
  QVERIFY2(b.ok, qPrintable(b.error));
  QCOMPARE(a.labels, b.labels);
  QCOMPARE(a.confidence, b.confidence);
  QCOMPARE(a.squaredDistance, b.squaredDistance);
  QCOMPARE(a.provenance.value("normalizationSource").toString(),
           QString("model_training_raw_units"));
  QVERIFY(a.provenance.value("standardizeRequestIgnored").toBool());
  // 对称方向：模型 standardize=true + 请求 standardize=false → 仍按训练
  // 口径标准化，normalizationSource 记 model_normalization_stats。
  ClassificationOptions standardized;
  standardized.method = Classifier::Lda;
  standardized.cvFolds = 2;
  TrainedModel stdModel;
  QVERIFY2(FaciesTrainingService::train(s, t, standardized, &stdModel, &error),
           qPrintable(error));
  QVERIFY(stdModel.provenance.contains("normalizationMean"));
  ClassificationOptions noStandardize = standardized;
  noStandardize.standardize = false;
  const auto c =
      FaciesTrainingService::classifyWith(s, stdModel, noStandardize);
  const auto d = FaciesTrainingService::classifyWith(s, stdModel, standardized);
  QVERIFY2(c.ok, qPrintable(c.error));
  QCOMPARE(c.labels, d.labels);
  QCOMPARE(c.confidence, d.confidence);
  QCOMPARE(c.squaredDistance, d.squaredDistance);
  QCOMPARE(c.provenance.value("normalizationSource").toString(),
           QString("model_normalization_stats"));
  QVERIFY(c.provenance.value("standardizeRequestIgnored").toBool());
  // 手搓模型：训练口径记为标准化但统计量缺失 → 如实拒答，绝不重估
  TrainedModel broken = m;
  broken.provenance.insert("standardize", true);
  const auto missing =
      FaciesTrainingService::classifyWith(s, broken, withStandardize);
  QVERIFY(!missing.ok);
  QVERIFY(missing.error.contains("统计量"));
  // 手搓模型：统计量含非有限值 / 非正尺度 → 中文拒答（堵英文兜底漏出）
  TrainedModel poison = stdModel;
  auto meanList = stdModel.provenance.value("normalizationMean").toList();
  auto sdList = stdModel.provenance.value("normalizationSd").toList();
  meanList[0] = std::numeric_limits<double>::quiet_NaN();
  poison.provenance.insert("normalizationMean", meanList);
  poison.provenance.insert("normalizationSd", sdList);
  const auto poisoned =
      FaciesTrainingService::classifyWith(s, poison, withStandardize);
  QVERIFY(!poisoned.ok);
  QVERIFY(poisoned.error.contains("非有限值"));
  TrainedModel zeroScale = stdModel;
  auto sdZero = sdList;
  sdZero[1] = 0.0;
  zeroScale.provenance.insert("normalizationSd", sdZero);
  const auto zeroed =
      FaciesTrainingService::classifyWith(s, zeroScale, withStandardize);
  QVERIFY(!zeroed.ok);
  QVERIFY(zeroed.error.contains("非正尺度"));
  // 手搓模型：provenance 缺 standardize 标志 → 拒
  TrainedModel flagless = m;
  flagless.provenance.remove("standardize");
  const auto noFlag = FaciesTrainingService::classifyWith(s, flagless, raw);
  QVERIFY(!noFlag.ok);
  QVERIFY(noFlag.error.contains("标准化口径"));
}
void TestFaciesSupervised::supervisedGate() {
  // 监督方法直接调 service classify → 如实拒答
  const SampleSet s = twoClassGrid(4, 4, 1, 4, 7);
  for (Classifier m : {Classifier::Lda, Classifier::Qda, Classifier::Knn}) {
    ClassificationOptions o;
    o.method = m;
    const auto r = FaciesClassificationService::classify(s, o);
    QVERIFY(!r.ok);
    QVERIFY(r.error.contains("FaciesTrainingService"));
  }
  // 训练方法不一致：LDA 模型不喂给 QDA 分类
  const TrainingSet t = twoClassLabels(s);
  ClassificationOptions lda;
  lda.method = Classifier::Lda;
  lda.cvFolds = 2;
  TrainedModel m;
  QString error;
  QVERIFY(FaciesTrainingService::train(s, t, lda, &m, &error));
  ClassificationOptions qda;
  qda.method = Classifier::Qda;
  qda.cvFolds = 2;
  const auto mismatch = FaciesTrainingService::classifyWith(s, m, qda);
  QVERIFY(!mismatch.ok);
  QVERIFY(!mismatch.error.isEmpty());
  // classifyWith 遇非监督方法 → 独立拒因
  ClassificationOptions kmeans;
  kmeans.method = Classifier::KMeans;
  const auto notSupervised =
      FaciesTrainingService::classifyWith(s, m, kmeans);
  QVERIFY(!notSupervised.ok);
  QVERIFY(notSupervised.error.contains("不是监督分类器"));
}
int main(int argc, char **argv) {
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", "/usr"), true);
  app.initQgis();
  GDALAllRegister();
  TestFaciesSupervised test;
  int rc = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}
#include "tst_faciessupervised.moc"
