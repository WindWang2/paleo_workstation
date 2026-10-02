#include "algorithms/cluster/cluster.h"
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
using namespace paleo::cluster;
class TestCluster : public QObject {
  Q_OBJECT
private slots:
  void bimodal();
  void degenerate();
  void gaussian();
  void rules();
  void cancellation();
};
void TestCluster::bimodal() {
  Matrix x{3, {}};
  std::mt19937_64 rng(13);
  std::normal_distribution<double> noise(0, .1);
  for (int i = 0; i < 4000; ++i)
    for (int j = 0; j < 3; ++j)
      x.values.push_back((i < 2000 ? -3 : 3) + noise(rng));
  auto r = kmeans(x);
  QVERIFY2(r.ok, r.error.c_str());
  QCOMPARE(r.model.centers.rows(), std::size_t(2));
  int correct = 0;
  const int negative = r.model.centers.values[0] < 0 ? 0 : 1;
  for (int i = 0; i < 4000; ++i)
    correct +=
        (r.labels[std::size_t(i)] == (i < 2000 ? negative : 1 - negative));
  QVERIFY(double(correct) / 4000 > .95);
  for (double c : r.model.centers.values)
    QVERIFY(std::abs(std::abs(c) - 3) / 3 < .05);
  for (std::size_t i = 1; i < r.objectiveHistory.size(); ++i)
    QVERIFY(r.objectiveHistory[i] <= r.objectiveHistory[i - 1] + 1e-8);
  Options one;
  one.k = 1;
  r = kmeans(x, one);
  QVERIFY(r.ok);
  for (std::size_t j = 0; j < 3; ++j) {
    double mean = 0;
    for (std::size_t i = 0; i < x.rows(); ++i)
      mean += x.row(i)[j];
    mean /= double(x.rows());
    QVERIFY(std::abs(r.model.centers.values[j] - mean) < 1e-12);
  }
}
void TestCluster::degenerate() {
  for (const Matrix &x :
       std::vector<Matrix>{{2, {}}, {2, {5, 7}}, {2, {5, 7, 5, 7, 5, 7}}}) {
    auto r = kmeans(x);
    QVERIFY(r.ok);
    QCOMPARE(r.labels.size(), x.rows());
    QCOMPARE(r.model.centers.rows(),
             x.rows() ? std::size_t(1) : std::size_t(0));
    auto g = gmm(x);
    QVERIFY2(g.ok, g.error.c_str());
    QCOMPARE(g.labels.size(), x.rows());
  }
  QVERIFY(!kmeans({0, {1}}).ok);
  QVERIFY(!kmeans({2, {1}}).ok);
  QVERIFY(!kmeans({1, {std::numeric_limits<double>::quiet_NaN()}}).ok);
  Options bad;
  bad.k = 0;
  QVERIFY(!gmm({1, {1}}, bad).ok);
  QVERIFY(!predict({1, {1}}, {Method::Gmm, {1, {1}}, {0}, {1}}).ok);
}
void TestCluster::gaussian() {
  Matrix x{2, {}};
  std::mt19937_64 rng(7);
  std::normal_distribution<double> noise(0, .7);
  for (int i = 0; i < 10000; ++i) {
    double c = i < 3000 ? -4 : 4;
    x.values.push_back(c + noise(rng));
    x.values.push_back(c * 2 + noise(rng));
  }
  auto r = gmm(x);
  QVERIFY2(r.ok, r.error.c_str());
  int neg = r.model.centers.values[0] < 0 ? 0 : 1;
  for (int c = 0; c < 2; ++c) {
    double mean = c == neg ? -4 : 4;
    double weight = c == neg ? .3 : .7;
    QVERIFY(std::abs(r.model.weights[std::size_t(c)] - weight) / weight < .1);
    for (int j = 0; j < 2; ++j)
      QVERIFY(std::abs(r.model.centers.values[std::size_t(c * 2 + j)] -
                       mean * (j + 1)) /
                  std::abs(mean * (j + 1)) <
              .1);
  }
  for (std::size_t i = 1; i < r.objectiveHistory.size(); ++i)
    QVERIFY(r.objectiveHistory[i] + 1e-7 >= r.objectiveHistory[i - 1]);
  const double expected = 9 * std::log(10000.) - 2 * r.objectiveHistory.back();
  QVERIFY(std::abs(r.bic - expected) < 1e-7);
  Options one;
  one.k = 1;
  auto single = gmm(x, one);
  QVERIFY(single.ok);
  QVERIFY(r.bic < single.bic);
  for (double c : r.confidence)
    QVERIFY(c >= .5 && c <= 1);
}
void TestCluster::rules() {
  std::vector<Point> v{{0, 0}, {2, 0}, {2, 2}, {0, 2}, {1, 1}};
  auto hull = convexHull(v);
  QCOMPARE(hull.size(), std::size_t(4));
  QVERIFY(inPolygon({1, 1}, hull));
  QVERIFY(inPolygon({0, 1}, hull));
  QVERIFY(inPolygon({2, 2}, hull));
  QVERIFY(!inPolygon({2.01, 1}, hull));
  std::vector<Point> lasso{{0, 0}, {3, 0}, {3, 1}, {1, 1}, {1, 3}, {0, 3}};
  QVERIFY(!inPolygon({2, 2}, lasso));
  QVERIFY(inPolygon({.5, 2}, lasso));
  QVERIFY(inPolygon({1, 2}, lasso));
  QVERIFY(inPolygon({1, 0}, convexHull({{0, 0}, {1, 0}, {2, 0}})));
  QVERIFY(!inPolygon({1, .01}, {{0, 0}, {2, 0}}));
  double p[]{1, 2, 3};
  QVERIFY(inBox(p, {1, 1, 1}, {2, 2, 3}));
  QVERIFY(!inBox(p, {1, 1, 1}, {2, 2, 2}));
}
void TestCluster::cancellation() {
  int count = 0;
  Control ctl{[&] { return ++count > 2; }, {}};
  Matrix x{2, std::vector<double>(10000, 1)};
  auto r = kmeans(x, {}, ctl);
  QVERIFY(r.cancelled);
  QVERIFY(!r.ok);
  QVERIFY(r.labels.empty());
  double last = -1;
  ctl = {[] { return false; },
         [&](double p) {
           QVERIFY(p >= last);
           last = p;
         }};
  r = gmm(x, {}, ctl);
  QVERIFY(r.ok);
  QCOMPARE(last, 1.);
}
QTEST_APPLESS_MAIN(TestCluster)
#include "tst_cluster.moc"
