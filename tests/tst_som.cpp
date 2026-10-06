#include "algorithms/cluster/som.h"
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
using namespace paleo::cluster;
namespace {
// Two tight blobs on the x axis; centres 12 sigma apart (6 sigma off the
// origin, noise sigma = 0.5).
Matrix twoBlobs(int perBlob, double centre, double noise, std::uint64_t seed) {
  Matrix x{2, {}};
  std::mt19937_64 rng(seed);
  std::normal_distribution<double> gauss(0, noise);
  for (int b = 0; b < 2; ++b)
    for (int i = 0; i < perBlob; ++i) {
      const double c = b ? centre : -centre;
      x.values.push_back(c + gauss(rng));
      x.values.push_back(gauss(rng));
    }
  return x;
}
double dist(const double *a, const double *b) {
  return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) +
                   (a[1] - b[1]) * (a[1] - b[1]));
}
const double *proto(const SomResult &r, int u) {
  return r.prototypes.values.data() + std::size_t(u) * 2;
}
// Labels of one blob must fill one contiguous block of prototype slots
// without gaps; the block bounds come back through lo/hi.
void labelSpan(const std::vector<int> &labels, int first, int count, int units,
               int &lo, int &hi) {
  std::vector<bool> seen(std::size_t(units), false);
  for (int i = first; i < first + count; ++i) {
    QVERIFY(labels[std::size_t(i)] >= 0);
    QVERIFY(labels[std::size_t(i)] < units);
    seen[std::size_t(labels[std::size_t(i)])] = true;
  }
  lo = hi = -1;
  for (int i = 0; i < units; ++i)
    if (seen[std::size_t(i)]) {
      if (lo < 0)
        lo = i;
      hi = i;
    }
  QVERIFY(lo >= 0);
  for (int i = lo; i <= hi; ++i)
    QVERIFY(seen[std::size_t(i)]);
}
} // namespace
class TestSom : public QObject {
  Q_OBJECT
private slots:
  void topology();
  void mapping();
  void determinism();
  void denseGrid();
  void convergence();
  void degenerate();
  void outputs();
  void cancellation();
};
void TestSom::topology() {
  const Matrix x = twoBlobs(400, 3, .5, 21);
  SomOptions line;
  line.width = 8;
  line.height = 1;
  auto r = som(x, line);
  QVERIFY2(r.ok, r.error.c_str());
  QCOMPARE(r.prototypes.rows(), std::size_t(8));
  QCOMPARE(r.width, 8);
  QCOMPARE(r.height, 1);
  for (int i = 0; i + 4 < 8; ++i)
    QVERIFY(dist(proto(r, i), proto(r, i + 1)) <
            dist(proto(r, i), proto(r, i + 4)));
  double adjacent = 0;
  for (int i = 0; i < 7; ++i)
    adjacent += dist(proto(r, i), proto(r, i + 1));
  adjacent /= 7;
  QVERIFY(adjacent < dist(proto(r, 0), proto(r, 7)) / 2);

  SomOptions grid;
  grid.width = 4;
  grid.height = 4;
  auto g = som(x, grid);
  QVERIFY2(g.ok, g.error.c_str());
  QCOMPARE(g.prototypes.rows(), std::size_t(16));
  double near = 0;
  int nearCount = 0;
  for (int y = 0; y < 4; ++y)
    for (int xx = 0; xx < 4; ++xx) {
      const auto at = [&g](int a, int b) { return proto(g, b * 4 + a); };
      if (xx + 1 < 4) {
        near += dist(at(xx, y), at(xx + 1, y));
        ++nearCount;
      }
      if (y + 1 < 4) {
        near += dist(at(xx, y), at(xx, y + 1));
        ++nearCount;
      }
    }
  near /= nearCount;
  const int pairs[4][2] = {{0, 15}, {3, 12}, {0, 12}, {3, 15}};
  double far = 0;
  for (const auto &pair : pairs)
    far += dist(proto(g, pair[0]), proto(g, pair[1]));
  far /= 4;
  QVERIFY(near < far);
}
// The map must separate the two blobs, not merely be smooth: each blob's
// samples land in a contiguous, disjoint block of the 8x1 line, and the 4x4
// grid centroids of the two blobs stay far apart in grid space.
void TestSom::mapping() {
  const Matrix x = twoBlobs(400, 3, .5, 21);
  SomOptions line;
  line.width = 8;
  line.height = 1;
  auto r = som(x, line);
  QVERIFY2(r.ok, r.error.c_str());
  int aLo, aHi, bLo, bHi;
  labelSpan(r.labels, 0, 400, 8, aLo, aHi);
  labelSpan(r.labels, 400, 400, 8, bLo, bHi);
  QVERIFY(aHi < bLo || bHi < aLo); // either blob may lead the line

  SomOptions grid;
  grid.width = 4;
  grid.height = 4;
  auto g = som(x, grid);
  QVERIFY2(g.ok, g.error.c_str());
  double ax = 0, ay = 0, bx = 0, by = 0;
  for (int i = 0; i < 400; ++i) {
    ax += g.labels[std::size_t(i)] % 4;
    ay += g.labels[std::size_t(i)] / 4;
  }
  for (std::size_t i = 400; i < g.labels.size(); ++i) {
    bx += g.labels[i] % 4;
    by += g.labels[i] / 4;
  }
  ax /= 400;
  ay /= 400;
  bx /= 400;
  by /= 400;
  QVERIFY(std::hypot(ax - bx, ay - by) >
          std::sqrt(18.0) / 3); // grid diagonal / 3
}
void TestSom::determinism() {
  const Matrix x = twoBlobs(120, 3, .5, 5);
  SomOptions o;
  o.width = 3;
  o.height = 2;
  o.seed = 99;
  const auto a = som(x, o);
  const auto b = som(x, o);
  QVERIFY2(a.ok, a.error.c_str());
  QVERIFY(b.ok);
  QCOMPARE(a.prototypes.values.size(), b.prototypes.values.size());
  QVERIFY(std::memcmp(a.prototypes.values.data(), b.prototypes.values.data(),
                      a.prototypes.values.size() * sizeof(double)) == 0);
  QVERIFY(a.labels == b.labels);
  QVERIFY(a.quantizationErrorHistory == b.quantizationErrorHistory);
  SomOptions other = o;
  other.seed = 98;
  const auto c = som(x, other);
  QVERIFY(c.ok);
  QVERIFY(a.prototypes.values != c.prototypes.values); // the seed takes effect
}
// 4x4 grid outgrows n=8: the extra prototypes repeat draws with replacement
// (som.cpp init) and training still produces a legal output face.
void TestSom::denseGrid() {
  const Matrix x = twoBlobs(4, 3, .5, 33); // 8 samples
  SomOptions o;
  o.width = 4;
  o.height = 4;
  o.epochs = 20;
  auto r = som(x, o);
  QVERIFY2(r.ok, r.error.c_str());
  QCOMPARE(r.prototypes.rows(), std::size_t(16));
  QCOMPARE(r.labels.size(), x.rows());
  QCOMPARE(r.confidence.size(), x.rows());
  QCOMPARE(r.squaredDistance.size(), x.rows());
  QCOMPARE(r.quantizationErrorHistory.size(), std::size_t(20));
  for (std::size_t i = 0; i < x.rows(); ++i) {
    QVERIFY(r.labels[i] >= 0);
    QVERIFY(r.labels[i] < 16);
    QVERIFY(r.confidence[i] >= 0);
    QVERIFY(r.confidence[i] <= 1);
    QVERIFY(r.squaredDistance[i] >= 0);
  }
  // The with-replacement init branch must stay bit-for-bit deterministic.
  const auto again = som(x, o);
  QVERIFY2(again.ok, again.error.c_str());
  QCOMPARE(again.prototypes.values.size(), r.prototypes.values.size());
  QVERIFY(std::memcmp(r.prototypes.values.data(),
                      again.prototypes.values.data(),
                      r.prototypes.values.size() * sizeof(double)) == 0);
  QVERIFY(r.labels == again.labels);
  QVERIFY(r.quantizationErrorHistory == again.quantizationErrorHistory);
}
void TestSom::convergence() {
  const Matrix x = twoBlobs(300, 3, .5, 11);
  auto r = som(x);
  QVERIFY2(r.ok, r.error.c_str());
  QCOMPARE(r.quantizationErrorHistory.size(), std::size_t(50));
  QVERIFY(r.quantizationErrorHistory.back() <
          r.quantizationErrorHistory.front());
}
void TestSom::degenerate() {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const Matrix &bad : std::vector<Matrix>{
           {0, {}}, {2, {}}, {0, {1, 2}}, {2, {1}}, {2, {1, nan, 1, 1}}}) {
    auto r = som(bad);
    QVERIFY(!r.ok);
    QVERIFY(!r.error.empty());
    QVERIFY(r.labels.empty());
  }
  SomOptions bad;
  bad.width = 0;
  QVERIFY(!som({2, {5, 7, 5, 7}}, bad).ok);
  bad.width = 4;
  bad.epochs = 0;
  QVERIFY(!som({2, {5, 7, 5, 7}}, bad).ok);
  SomOptions huge;
  huge.width = 1001;
  huge.height = 1001; // 1'002'001 cells > the 1'000'000 cap
  auto oversized = som({2, {5, 7, 5, 7}}, huge);
  QVERIFY(!oversized.ok);
  QVERIFY(!oversized.error.empty());
  SomOptions wide;
  wide.width = 1000;
  wide.height = 1000; // 1e6 cells x 101 dims exceeds the prototype budget
  auto overBudget = som({101, std::vector<double>(101, 1)}, wide);
  QVERIFY(!overBudget.ok);
  QVERIFY(!overBudget.error.empty());
}
void TestSom::outputs() {
  const Matrix x = twoBlobs(150, 3, .5, 3);
  SomOptions o;
  o.width = 2;
  o.height = 2;
  double last = -1;
  int reports = 0;
  Control ctl{{}, [&](double p) {
                QVERIFY(p >= last);
                last = p;
                ++reports;
              }};
  auto r = som(x, o, ctl);
  QVERIFY2(r.ok, r.error.c_str());
  QCOMPARE(last, 1.);
  QVERIFY(reports > 0);
  QCOMPARE(r.labels.size(), x.rows());
  QCOMPARE(r.confidence.size(), x.rows());
  QCOMPARE(r.squaredDistance.size(), x.rows());
  QCOMPARE(r.prototypes.dimensions, x.dimensions);
  QCOMPARE(r.prototypes.values.size(), std::size_t(8));
  for (std::size_t i = 0; i < x.rows(); ++i) {
    const int label = r.labels[i];
    QVERIFY(label >= 0);
    QVERIFY(label < 4);
    QVERIFY(r.confidence[i] >= 0);
    QVERIFY(r.confidence[i] <= 1);
    QVERIFY(r.squaredDistance[i] >= 0);
    const double d = dist(x.row(i), proto(r, label));
    QVERIFY(std::abs(d * d - r.squaredDistance[i]) < 1e-9);
  }
  SomOptions one;
  one.width = 1;
  one.height = 1;
  one.epochs = 5;
  auto single = som(x, one);
  QVERIFY2(single.ok, single.error.c_str());
  for (double c : single.confidence)
    QCOMPARE(c, 1.);
}
void TestSom::cancellation() {
  int count = 0;
  Control ctl{[&] { return ++count > 2; }, {}};
  const Matrix x = twoBlobs(400, 3, .5, 8);
  auto r = som(x, {}, ctl);
  QVERIFY(r.cancelled);
  QVERIFY(!r.ok);
  QVERIFY(r.labels.empty());
  QVERIFY(!r.error.empty());
  Control instant{[] { return true; }, {}};
  r = som(x, {}, instant);
  QVERIFY(r.cancelled);
  QVERIFY(!r.ok);
  QVERIFY(r.labels.empty());
}
QTEST_APPLESS_MAIN(TestSom)
#include "tst_som.moc"
