#include "algorithms/cluster/cluster.h"
#include "algorithms/cluster/cluster_internal.h"
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
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
  void randomStreamConvention();
  void randomPrimitivesMatchStdStatistics();
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
  QVERIFY(
      !predict(
           {1, {1}},
           {Method::Gmm, {1, {1}}, {std::numeric_limits<double>::max()}, {1}})
           .ok);
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
void TestCluster::randomStreamConvention() {
  // #470：cluster 域随机口径钉位——抽样必须走 mt19937_64 原始 u64 的自订
  // 推导（与 geostat/sgs 同口径：取模整数 + 53bit (0,1) 单值 + Fisher–
  // Yates），不得用序列为实现定义的 std::uniform_*/std::shuffle。
  // n=k=3 的可分小集上 kmeans++ 恰好消耗三次随机数，Lloyd 一轮收敛后
  // 标签排列完整暴露随机流——与测试内裸流参考实现逐位对拍。
  // 修复前（std::uniform_int/real_distribution）本测试在 libstdc++ 上红。
  const Matrix x{1, {0, 10, 100}};
  const double v[3] = {0, 10, 100};
  for (std::uint64_t seed : {1ULL, 7ULL, 42ULL, 99ULL, 20261016ULL}) {
    Options o;
    o.k = 3;
    o.seed = seed;
    auto r = kmeans(x, o);
    QVERIFY2(r.ok, r.error.c_str());
    QCOMPARE(r.model.centers.rows(), std::size_t(3));
    // 参考流：首中心 rng()%3；第 2/3 步各耗一次 53bit (0,1) 单值
    //（第 3 步的最近距和只剩单项，target∈(0,sum) 必中，但仍要耗流）。
    std::mt19937_64 rng(seed);
    const auto unit = [&rng] {
      return (double(rng() >> 11) + 0.5) * 0x1.0p-53;
    };
    bool taken[3] = {false, false, false};
    std::vector<int> pick;
    pick.push_back(int(rng() % 3));
    taken[size_t(pick[0])] = true;
    for (int c = 1; c < 3; ++c) {
      double nearest[3] = {0, 0, 0};
      double sum = 0;
      for (int i = 0; i < 3; ++i) {
        if (taken[i])
          continue;
        double best = std::numeric_limits<double>::infinity();
        for (int p : pick) {
          const double d = v[i] - v[p];
          best = std::min(best, d * d);
        }
        nearest[i] = best;
        sum += best;
      }
      const double target = unit() * sum;
      double cumulative = 0;
      int picked = 2;
      for (int i = 0; i < 3; ++i) {
        cumulative += nearest[i];
        if (cumulative > target) {
          picked = i;
          break;
        }
      }
      taken[size_t(picked)] = true;
      pick.push_back(picked);
    }
    std::vector<int> expected(3);
    for (int pos = 0; pos < 3; ++pos)
      expected[size_t(pick[size_t(pos)])] = pos;
    QCOMPARE(r.labels, expected);
    // 同 seed 二次运行逐位一致（som/supervised 的同款口径已有各自 determinism 测试）。
    auto again = kmeans(x, o);
    QVERIFY(again.ok);
    QCOMPARE(again.labels, r.labels);
    QCOMPARE(again.model.centers.values, r.model.centers.values);
  }
}
void TestCluster::randomPrimitivesMatchStdStatistics() {
  // #470：自订随机原语的三面守——(1) 同 seed 两次运行逐位一致；
  // (2) 与被替换的 std::uniform_*（本平台参考实现）统计性质对拍
  //（均值/方差容差：20 万抽样的标准误 << 容差）；(3) 洗牌是排列且
  // 位置分布均匀（每元素落每位 ≈ (n-1)/2）。
  {
    std::mt19937_64 a(5), b(5);
    for (int i = 0; i < 1000; ++i) {
      QCOMPARE(randomBelow(a, 97), randomBelow(b, 97));
      QCOMPARE(unitRandom(a), unitRandom(b));
    }
  }
  {
    const std::size_t n = 97, draws = 200000;
    double sumNew = 0, sumSqNew = 0;
    std::mt19937_64 rng(11);
    for (std::size_t i = 0; i < draws; ++i) {
      const double v = double(randomBelow(rng, n));
      sumNew += v;
      sumSqNew += v * v;
    }
    const double mean = sumNew / draws;
    const double var = sumSqNew / draws - mean * mean;
    QVERIFY(std::abs(mean - (n - 1) / 2.0) < 0.5);
    QVERIFY(std::abs(var - (n * n - 1) / 12.0) < 30.0);
    // std 参考对拍（同分布口径的均值差）
    std::mt19937_64 ref(11);
    std::uniform_int_distribution<std::size_t> refInt(0, n - 1);
    double sumOld = 0;
    for (std::size_t i = 0; i < draws; ++i)
      sumOld += double(refInt(ref));
    QVERIFY(std::abs(mean - sumOld / draws) < 0.5);
  }
  {
    const std::size_t draws = 200000;
    double sum = 0, sumSq = 0;
    std::mt19937_64 rng(13);
    for (std::size_t i = 0; i < draws; ++i) {
      const double v = unitRandom(rng);
      sum += v;
      sumSq += v * v;
    }
    const double mean = sum / draws;
    const double var = sumSq / draws - mean * mean;
    QVERIFY(mean > 0 && mean < 1); // (0,1) 开区间（53bit 尾数 + 半格偏置）
    QVERIFY(std::abs(mean - 0.5) < 0.01);
    QVERIFY(std::abs(var - 1.0 / 12.0) < 0.01);
    std::mt19937_64 ref(13);
    std::uniform_real_distribution<double> refReal(0, 1);
    double sumOld = 0;
    for (std::size_t i = 0; i < draws; ++i)
      sumOld += refReal(ref);
    QVERIFY(std::abs(mean - sumOld / draws) < 0.01);
  }
  {
    const std::size_t n = 8, rounds = 20000;
    std::vector<double> positionSum(n, 0);
    std::mt19937_64 rng(17);
    for (std::size_t r = 0; r < rounds; ++r) {
      std::vector<std::size_t> seq(n);
      std::iota(seq.begin(), seq.end(), std::size_t(0));
      shuffleDeterministic(seq.begin(), seq.end(), rng);
      std::vector<bool> seen(n, false);
      for (std::size_t p = 0; p < n; ++p) {
        QVERIFY(seq[p] < n);
        QVERIFY(!seen[seq[p]]); // 是排列（无重复）
        seen[seq[p]] = true;
        positionSum[seq[p]] += double(p);
      }
    }
    for (std::size_t e = 0; e < n; ++e) {
      const double meanPos = positionSum[e] / rounds;
      QVERIFY(std::abs(meanPos - (n - 1) / 2.0) < 0.2);
    }
  }
}
QTEST_APPLESS_MAIN(TestCluster)
#include "tst_cluster.moc"
