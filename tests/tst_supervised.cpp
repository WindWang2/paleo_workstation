#include "algorithms/cluster/supervised.h"
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
using namespace paleo::cluster;
namespace {
// Two isotropic Gaussian classes on the main diagonal. `separation` is the
// center-to-center distance per coordinate, so with sigma = spread it equals
// separation / spread sigmas (4 sigma when separation = 4, spread = 1).
// Rows alternate between the classes, so labels stay interleaved in the
// sample order rather than blocked by class.
Matrix twoClusters(int perClass, double spread, double separation,
                   std::uint64_t seed, std::vector<int> &labels) {
  Matrix x{2, {}};
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> noise(0, spread);
  for (int i = 0; i < perClass; ++i)
    for (int cls = 0; cls < 2; ++cls) {
      const double center = (cls ? 0.5 : -0.5) * separation;
      x.values.push_back(center + noise(rng));
      x.values.push_back(center + noise(rng));
      labels.push_back(cls);
    }
  return x;
}
double mean(const std::vector<double> &v) {
  return std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
}
bool finiteAll(const std::vector<double> &v) {
  return std::all_of(v.begin(), v.end(),
                     [](double x) { return std::isfinite(x); });
}
} // namespace
class TestSupervised : public QObject {
  Q_OBJECT
private slots:
  void separable();
  void oracle();
  void golden();
  void interleaved();
  void degenerate();
  void honesty();
  void determinism();
  void knn();
  void cancelledContract();
};
void TestSupervised::separable() {
  std::vector<int> labels;
  // Centers +-2: mean distance 4 = 4 sigma per axis. Bayes recall is about
  // Phi(-2) ~ 0.977, so the 0.95 in-sample and 0.9 cross-validation gates
  // keep margin.
  const Matrix x = twoClusters(1000, 1, 4, 11, labels);
  for (SupervisedMethod method :
       {SupervisedMethod::Lda, SupervisedMethod::Qda,
        SupervisedMethod::Knn}) {
    SupervisedOptions o;
    o.method = method;
    auto r = trainSupervised(x, labels, o);
    QVERIFY2(r.ok, r.error.c_str());
    QCOMPARE(r.labels.size(), x.rows());
    QCOMPARE(r.confidence.size(), x.rows());
    QVERIFY(finiteAll(r.confidence));
    int correct = 0;
    for (std::size_t i = 0; i < x.rows(); ++i)
      correct += r.labels[i] == labels[i];
    QVERIFY(double(correct) / double(x.rows()) > 0.95);
    QCOMPARE(r.model.classIds, (std::vector<int>{0, 1}));
    QCOMPARE(r.model.dimensions, std::size_t(2));
    if (method == SupervisedMethod::Knn) {
      QCOMPARE(r.model.trainValues.rows(), x.rows());
      QCOMPARE(r.model.trainLabels.size(), x.rows());
      QVERIFY(r.model.means.values.empty());
      QVERIFY(r.model.covariances.empty());
      QCOMPARE(r.model.knnNeighbors, 5);
    } else {
      QCOMPARE(r.model.means.rows(), std::size_t(2));
      for (double p : r.model.priors)
        QVERIFY(std::abs(p - 0.5) < 0.02);
      for (std::size_t k = 0; k < 2; ++k)
        for (std::size_t j = 0; j < 2; ++j)
          QVERIFY(std::abs(r.model.means.values[k * 2 + j] -
                           (k ? 2 : -2)) < 0.15);
      const std::size_t want =
          method == SupervisedMethod::Lda ? 4 : 2 * 4;
      QCOMPARE(r.model.covariances.size(), want);
      for (std::size_t k = 0; k < want / 4; ++k) {
        for (std::size_t j = 0; j < 2; ++j)
          QVERIFY(r.model.covariances[k * 4 + j * 2 + j] > 0.5);
        QVERIFY(std::abs(r.model.covariances[k * 4 + 1]) < 0.5);
        QVERIFY(r.model.covariances[k * 4 + 1] ==
                r.model.covariances[k * 4 + 2]); // symmetric
      }
    }
    auto cv = crossValidate(x, labels, o);
    QVERIFY2(cv.ok, cv.error.c_str());
    QCOMPARE(cv.folds, 5);
    for (int k = 0; k < 2; ++k) {
      QVERIFY(cv.confusion.precision[std::size_t(k)] >= 0.9);
      QVERIFY(cv.confusion.recall[std::size_t(k)] >= 0.9);
    }
    std::int64_t total = 0;
    for (const auto &row : cv.confusion.cells)
      for (std::int64_t v : row)
        total += v;
    QCOMPARE(total, std::int64_t(x.rows()));
  }
}
void TestSupervised::oracle() {
  std::vector<int> labels;
  const Matrix x = twoClusters(200, 1, 4, 23, labels);
  Matrix holes{2, x.values}; // dense matrix, some labels blanked to -1
  std::vector<int> holeLabels(labels);
  for (std::size_t i = 0; i < holeLabels.size(); ++i)
    if (i % 10 == 4)
      holeLabels[i] = -1;
  std::vector<std::size_t> labeledRows;
  for (std::size_t i = 0; i < holeLabels.size(); ++i)
    if (holeLabels[i] != -1)
      labeledRows.push_back(i);
  for (SupervisedMethod method :
       {SupervisedMethod::Lda, SupervisedMethod::Qda,
        SupervisedMethod::Knn}) {
    SupervisedOptions o;
    o.method = method;
    auto trained = trainSupervised(x, labels, o);
    QVERIFY2(trained.ok, trained.error.c_str());
    auto predicted = predictSupervised(x, trained.model);
    QVERIFY2(predicted.ok, predicted.error.c_str());
    QCOMPARE(trained.labels, predicted.labels);
    for (std::size_t i = 0; i < trained.confidence.size(); ++i)
      QVERIFY(trained.confidence[i] == predicted.confidence[i]);
    auto holed = trainSupervised(holes, holeLabels, o);
    QVERIFY2(holed.ok, holed.error.c_str());
    QCOMPARE(holed.labels.size(), labeledRows.size());
    auto subset = predictSupervised(holes, holed.model);
    QVERIFY2(subset.ok, subset.error.c_str());
    QVERIFY(subset.labels.size() == holes.rows());
    for (std::size_t j = 0; j < labeledRows.size(); ++j) {
      QCOMPARE(holed.labels[j], subset.labels[labeledRows[j]]);
      QVERIFY(holed.confidence[j] ==
              subset.confidence[labeledRows[j]]);
    }
  }
}
void TestSupervised::golden() {
  // Integer fixture: class 0 = {(0,0),(2,0),(0,2)},
  // class 1 = {(4,4),(8,4),(4,8)}. Centers (2/3,2/3) and (16/3,16/3);
  // scatters S0 = [[8/3,-4/3],[-4/3,8/3]], S1 = [[32/3,-16/3],[-16/3,32/3]].
  const Matrix six{2, {0, 0, 2, 0, 0, 2, 4, 4, 8, 4, 4, 8}};
  const std::vector<int> sixLabels{0, 0, 0, 1, 1, 1};
  SupervisedOptions lda;
  auto l = trainSupervised(six, sixLabels, lda);
  QVERIFY2(l.ok, l.error.c_str());
  QCOMPARE(l.model.covariances.size(), std::size_t(4));
  // pooled scatter = (S0 + S1) / (n - c) = [[10/3, -5/3], [-5/3, 10/3]]
  QCOMPARE(l.model.covariances[0], 10.0 / 3.0 + 1e-6);
  QCOMPARE(l.model.covariances[1], -5.0 / 3.0);
  QCOMPARE(l.model.covariances[2], -5.0 / 3.0);
  QCOMPARE(l.model.covariances[3], 10.0 / 3.0 + 1e-6);
  QCOMPARE(l.model.means.values[0], 2.0 / 3.0);
  QCOMPARE(l.model.means.values[1], 2.0 / 3.0);
  QCOMPARE(l.model.means.values[2], 16.0 / 3.0);
  QCOMPARE(l.model.means.values[3], 16.0 / 3.0);
  QCOMPARE(l.model.priors[0], 0.5);
  QCOMPARE(l.model.priors[1], 0.5);
  SupervisedOptions qda;
  qda.method = SupervisedMethod::Qda;
  auto q = trainSupervised(six, sixLabels, qda);
  QVERIFY2(q.ok, q.error.c_str());
  QCOMPARE(q.model.covariances.size(), std::size_t(8));
  // per-class scatter = S_k / (n_k - 1)
  QCOMPARE(q.model.covariances[0], 4.0 / 3.0 + 1e-6);   // block 0
  QCOMPARE(q.model.covariances[1], -2.0 / 3.0);
  QCOMPARE(q.model.covariances[2], -2.0 / 3.0);
  QCOMPARE(q.model.covariances[3], 4.0 / 3.0 + 1e-6);
  QCOMPARE(q.model.covariances[4], 16.0 / 3.0 + 1e-6);  // block 1
  QCOMPARE(q.model.covariances[5], -8.0 / 3.0);
  QCOMPARE(q.model.covariances[6], -8.0 / 3.0);
  QCOMPARE(q.model.covariances[7], 16.0 / 3.0 + 1e-6);
  // LDA boundary: two samples per class gives exactly n - c == d degrees of
  // freedom, the smallest accepted case. Deviations are +-(0.5,0.5), so the
  // pooled scatter is [[0.5,0.5],[0.5,0.5]].
  const Matrix pair{2, {0, 0, 1, 1, 5, 5, 6, 6}};
  const std::vector<int> pairLabels{0, 0, 1, 1};
  auto b = trainSupervised(pair, pairLabels, lda);
  QVERIFY2(b.ok, b.error.c_str());
  QCOMPARE(b.model.covariances[0], 0.5 + 1e-6);
  QCOMPARE(b.model.covariances[1], 0.5);
  QCOMPARE(b.model.covariances[2], 0.5);
  QCOMPARE(b.model.covariances[3], 0.5 + 1e-6);
  // 1D symmetric fixture: classes {-3,-1} and {1,3}, equal priors and equal
  // spread, so the pooled scatter is 2 and the midpoint posterior is 0.5.
  const Matrix line{1, {-3, -1, 1, 3}};
  const std::vector<int> lineLabels{0, 0, 1, 1};
  auto t = trainSupervised(line, lineLabels, lda);
  QVERIFY2(t.ok, t.error.c_str());
  QCOMPARE(t.model.covariances.size(), std::size_t(1));
  QCOMPARE(t.model.covariances[0], 2.0 + 1e-6); // (S0 + S1) / (n - c)
  QCOMPARE(t.model.means.values[0], -2.0);
  QCOMPARE(t.model.means.values[1], 2.0);
  QCOMPARE(t.model.priors[0], 0.5);
  QCOMPARE(t.model.priors[1], 0.5);
  auto mid = predictSupervised(Matrix{1, {0}}, t.model);
  QVERIFY2(mid.ok, mid.error.c_str());
  QCOMPARE(mid.labels[0], 0); // posterior tie -> smaller classIds index
  QCOMPARE(mid.confidence[0], 0.5);
  QCOMPARE(mid.squaredDistance[0], 4.0 / (2.0 + 1e-6));
  auto own = predictSupervised(Matrix{1, {2}}, t.model);
  QVERIFY2(own.ok, own.error.c_str());
  QCOMPARE(own.labels[0], 1);
  QCOMPARE(own.squaredDistance[0], 0.0);
  const double expected =
      1.0 / (1.0 + std::exp(-0.5 * (16.0 / (2.0 + 1e-6))));
  QCOMPARE(own.confidence[0], expected);
  // Training-phase progress stays monotonic and ends at exactly 1.
  double last = -1;
  Control ctl{[] { return false; },
              [&](double p) {
                QVERIFY(p >= last);
                last = p;
              }};
  auto phased = trainSupervised(six, sixLabels, lda, ctl);
  QVERIFY2(phased.ok, phased.error.c_str());
  QCOMPARE(last, 1.0);
}
void TestSupervised::interleaved() {
  std::vector<int> labels;
  const Matrix x = twoClusters(400, 1, 4, 61, labels);
  for (std::size_t i = 0; i < labels.size(); ++i)
    QCOMPARE(labels[i], int(i % 2)); // labels alternate in the sample order
  SupervisedOptions lda;
  auto cv = crossValidate(x, labels, lda);
  QVERIFY2(cv.ok, cv.error.c_str());
  QCOMPARE(cv.folds, 5);
  for (int k = 0; k < 2; ++k) {
    QVERIFY(cv.confusion.precision[std::size_t(k)] >= 0.95);
    QVERIFY(cv.confusion.recall[std::size_t(k)] >= 0.95);
  }
  std::int64_t total = 0;
  for (const auto &row : cv.confusion.cells)
    for (std::int64_t v : row)
      total += v;
  QCOMPARE(total, std::int64_t(x.rows()));
}
void TestSupervised::degenerate() {
  const Matrix single{2, {0, 0, 1, 1, 2, 2, 3, 3}};
  const std::vector<int> oneClass{4, 4, 4, 4};
  const std::vector<int> allUnlabeled{-1, -1, -1, -1};
  const std::vector<int> mismatched{0, 1, 0};
  for (SupervisedMethod method :
       {SupervisedMethod::Lda, SupervisedMethod::Qda,
        SupervisedMethod::Knn}) {
    SupervisedOptions o;
    o.method = method;
    auto one = trainSupervised(single, oneClass, o);
    QVERIFY(!one.ok);
    QVERIFY(!one.error.empty());
    auto none = trainSupervised(single, allUnlabeled, o);
    QVERIFY(!none.ok);
    QVERIFY(!none.error.empty());
    auto mismatch = trainSupervised(single, mismatched, o);
    QVERIFY(!mismatch.ok);
  }
  const Matrix tiny{2, {0, 0, 1, 1, 2, 2, 3, 3, 4, 4}};
  const std::vector<int> singleton{0, 0, 0, 0, 1}; // a 1-sample class
  SupervisedOptions qda;
  qda.method = SupervisedMethod::Qda;
  auto q = trainSupervised(tiny, singleton, qda);
  QVERIFY(!q.ok);
  QVERIFY(!q.error.empty());
  SupervisedOptions lda;
  auto l = trainSupervised(tiny, singleton, lda);
  QVERIFY2(l.ok, l.error.c_str());
  QVERIFY(!trainSupervised({0, {}}, {0, 1}).ok);
  QVERIFY(!trainSupervised({2, {1, std::numeric_limits<double>::quiet_NaN()}},
                           {0, 1})
               .ok);
  const Matrix wide{4, {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2}};
  QVERIFY(!trainSupervised(wide, {0, 1, 0}, lda).ok); // n - c = 1 < d = 4
  QVERIFY(!trainSupervised({1, {0, 1}}, {0, 1}, lda).ok); // n - c = 0 < d = 1
  QVERIFY(!trainSupervised({2, {0, 0, 1, 1, 5, 5}}, {0, 0, 1}, lda)
               .ok); // n - c = 1 < d = 2
  const Matrix pair{2, {0, 0, 1, 1, 5, 5, 6, 6}};
  const std::vector<int> pairLabels{0, 0, 1, 1};
  QVERIFY(!trainSupervised(pair, pairLabels, qda).ok); // 2 < d + 1 = 3
  SupervisedOptions bad;
  bad.method = SupervisedMethod::Knn;
  bad.knnNeighbors = 0;
  QVERIFY(!trainSupervised(tiny, singleton, bad).ok);
  bad.knnNeighbors = 1;
  bad.varianceFloor = 0;
  QVERIFY(!trainSupervised(tiny, singleton, bad).ok);
  auto cv = crossValidate(tiny, singleton, lda);
  QVERIFY(!cv.ok);
  QVERIFY(!cv.error.empty());
  cv = crossValidate(tiny, singleton, lda, 1);
  QVERIFY(!cv.ok);
  // Smallest-class folds leave one sample per class in the training half, so
  // per-fold training fails and the whole run fails with a reason attached.
  auto small = crossValidate(pair, pairLabels, qda, 2);
  QVERIFY(!small.ok);
  QVERIFY(!small.error.empty());
  auto good = trainSupervised(tiny, singleton, lda);
  QVERIFY2(good.ok, good.error.c_str());
  auto wrong = predictSupervised(wide, good.model);
  QVERIFY(!wrong.ok);
  SupervisedModel broken = good.model;
  std::fill(broken.covariances.begin(), broken.covariances.end(), 0);
  QVERIFY(!predictSupervised(single, broken).ok);
  SupervisedOptions kopt;
  kopt.method = SupervisedMethod::Knn;
  auto kfit = trainSupervised(single, {0, 0, 1, 1}, kopt);
  QVERIFY2(kfit.ok, kfit.error.c_str());
  SupervisedModel emptyTrain = kfit.model;
  emptyTrain.trainValues = Matrix{2, {}};
  emptyTrain.trainLabels.clear();
  QVERIFY(!predictSupervised(single, emptyTrain).ok);
}
void TestSupervised::honesty() {
  for (SupervisedMethod method :
       {SupervisedMethod::Lda, SupervisedMethod::Qda,
        SupervisedMethod::Knn}) {
    SupervisedOptions o;
    o.method = method;
    std::vector<int> overlapLabels, apartLabels;
    // Centers +-0.5 per axis (Euclidean center distance sqrt(2) sigma): the
    // classes overlap heavily, so the least confident points sit at the ~0.5
    // boundary, far below the 0.9 honesty gate.
    const Matrix overlap = twoClusters(400, 1, 1, 31, overlapLabels);
    // Centers +-2: mean distance 4 = 4 sigma; the expected plug-in posterior
    // averages about Phi(2.79) ~ 0.997, above the 0.99 gate.
    const Matrix apart = twoClusters(400, 1, 4, 31, apartLabels);
    auto r = trainSupervised(overlap, overlapLabels, o);
    QVERIFY2(r.ok, r.error.c_str());
    QVERIFY(*std::min_element(r.confidence.begin(), r.confidence.end()) < 0.9);
    auto s = trainSupervised(apart, apartLabels, o);
    QVERIFY2(s.ok, s.error.c_str());
    QVERIFY(mean(s.confidence) >= 0.99);
    for (double c : s.confidence)
      QVERIFY(c >= 0 && c <= 1);
    for (double v : s.squaredDistance)
      QVERIFY(v >= 0);
  }
}
void TestSupervised::determinism() {
  std::vector<int> labels;
  const Matrix x = twoClusters(300, 1, 4, 47, labels);
  SupervisedOptions o;
  o.method = SupervisedMethod::Knn;
  const auto a = crossValidate(x, labels, o);
  const auto b = crossValidate(x, labels, o);
  QVERIFY2(a.ok, a.error.c_str());
  QVERIFY2(b.ok, b.error.c_str());
  QCOMPARE(a.folds, b.folds);
  QCOMPARE(a.confusion.classIds, b.confusion.classIds);
  QCOMPARE(a.confusion.cells, b.confusion.cells);
  QCOMPARE(a.confusion.precision, b.confusion.precision);
  QCOMPARE(a.confusion.recall, b.confusion.recall);
  const auto c = crossValidate(x, labels, o, 5, 99);
  QVERIFY2(c.ok, c.error.c_str());
  QVERIFY(a.confusion.cells != c.confusion.cells); // seed drives the folds
  SupervisedOptions qda;
  qda.method = SupervisedMethod::Qda;
  const auto d = crossValidate(x, labels, qda, 3, 99);
  const auto e = crossValidate(x, labels, qda, 3, 99);
  QVERIFY(d.ok && e.ok);
  QCOMPARE(d.confusion.cells, e.confusion.cells);
}
void TestSupervised::knn() {
  std::vector<int> labels;
  const Matrix x = twoClusters(300, 1, 4, 53, labels);
  SupervisedOptions o;
  o.method = SupervisedMethod::Knn;
  o.knnNeighbors = 1;
  auto cv = crossValidate(x, labels, o);
  QVERIFY2(cv.ok, cv.error.c_str());
  for (int k = 0; k < 2; ++k)
    QVERIFY(cv.confusion.recall[std::size_t(k)] >= 0.95);
  const Matrix tie{2, {0, 0, 2, 0}};
  const std::vector<int> tieLabels{7, 3}; // classIds sorted to [3, 7]
  SupervisedOptions tieOptions; // default k, clamped to the labeled rows
  tieOptions.method = SupervisedMethod::Knn;
  auto r = trainSupervised(tie, tieLabels, tieOptions);
  QVERIFY2(r.ok, r.error.c_str());
  QCOMPARE(r.model.classIds, (std::vector<int>{3, 7}));
  QCOMPARE(r.model.knnNeighbors, 2);
  const Matrix query{2, {1, 0}}; // equidistant from both training rows
  auto p = predictSupervised(query, r.model);
  QVERIFY2(p.ok, p.error.c_str());
  QCOMPARE(p.labels.back(), 0); // vote tie -> smaller classIds index wins
  QCOMPARE(p.confidence.back(), 0.5);
  QCOMPARE(p.squaredDistance.back(), 1.0);
  SupervisedOptions many;
  many.method = SupervisedMethod::Knn;
  many.knnNeighbors = 9; // clamped to the labeled row count
  auto big = trainSupervised(tie, tieLabels, many);
  QVERIFY2(big.ok, big.error.c_str());
  QCOMPARE(big.model.knnNeighbors, 2);
  QCOMPARE(big.labels.size(), std::size_t(2));
  for (double c : big.confidence)
    QCOMPARE(c, 0.5);
  std::vector<int> skewed;
  const Matrix few = twoClusters(2, 1, 4, 71, skewed);
  auto clamped = crossValidate(few, skewed, o, 5);
  QVERIFY2(clamped.ok, clamped.error.c_str());
  QCOMPARE(clamped.folds, 2); // clamped to the smallest class size
}
void TestSupervised::cancelledContract() {
  std::vector<int> labels;
  const Matrix x = twoClusters(40, 1, 4, 83, labels);
  SupervisedOptions lda;
  auto trained = trainSupervised(x, labels, lda);
  QVERIFY2(trained.ok, trained.error.c_str());
  Control stop{[] { return true; }, {}};
  auto refused = trainSupervised(x, labels, lda, stop);
  QVERIFY(!refused.ok);
  QVERIFY(refused.cancelled);
  QVERIFY(refused.labels.empty());
  auto predicted = predictSupervised(x, trained.model, stop);
  QVERIFY(!predicted.ok);
  QVERIFY(predicted.cancelled);
  QVERIFY(predicted.labels.empty());
  auto cv = crossValidate(x, labels, lda, 5, 42, stop);
  QVERIFY(!cv.ok);
  QVERIFY(cv.cancelled);
  QVERIFY(cv.confusion.cells.empty());
  for (SupervisedMethod method :
       {SupervisedMethod::Qda, SupervisedMethod::Knn}) {
    SupervisedOptions o;
    o.method = method;
    auto r = trainSupervised(x, labels, o, stop);
    QVERIFY(!r.ok);
    QVERIFY(r.cancelled);
    QVERIFY(r.labels.empty());
  }
}
QTEST_APPLESS_MAIN(TestSupervised)
#include "tst_supervised.moc"
