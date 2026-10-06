#include "algorithms/cluster/cluster.h"
#include <QtTest>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>
#if defined(__linux__)
#include <malloc.h>
#endif
using namespace paleo::cluster;
namespace {
// Process peak RSS (KB): Linux reads VmHWM from /proc/self/status; Windows
// uses GetProcessMemoryInfo's PeakWorkingSetSize (same lieu as
// tst_seismic_baseline.cpp).
qint64 peakRssKb() {
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    return qint64(pmc.PeakWorkingSetSize) / 1024;
  return -1;
#else
  std::ifstream status("/proc/self/status");
  std::string key;
  while (status >> key) {
    if (key == "VmHWM:") {
      qint64 kb = 0;
      status >> kb;
      return kb;
    }
    status.ignore(4096, '\n');
  }
  return -1;
#endif
}
void trimHeap() {
#if defined(__linux__)
  malloc_trim(0);
#endif
}
// Equal-weight 3-component 2D mixture, component spacing far beyond sigma
// (components separable).
Matrix separableMixture(std::size_t n, std::uint64_t seed, double sigma = .5) {
  Matrix x{2, {}};
  x.values.reserve(2 * n);
  std::mt19937_64 rng(seed);
  const double centers[3][2] = {{-6, 6}, {0, -6}, {6, 6}};
  std::normal_distribution<double> noise(0, sigma);
  std::uniform_int_distribution<int> pick(0, 2);
  for (std::size_t i = 0; i < n; ++i) {
    const double *c = centers[pick(rng)];
    x.values.push_back(c[0] + noise(rng));
    x.values.push_back(c[1] + noise(rng));
  }
  return x;
}
// Unequal weights/sigmas 3-component 2D mixture: center spacing >= 2.8 sigma
// (components separable) but close enough to drive multiple EM iterations
// (kmeans seeding sits away from the EM fixed point).
Matrix mixedMixture(std::size_t n, std::uint64_t seed) {
  Matrix x{2, {}};
  x.values.reserve(2 * n);
  std::mt19937_64 rng(seed);
  const double centers[3][2] = {{-3, 3}, {0, -3}, {3, 3}};
  const double sigma[3] = {.5, 1, 1.5};
  const double weight[3] = {.2, .3, .5};
  std::uniform_real_distribution<double> pick(0, 1);
  for (std::size_t i = 0; i < n; ++i) {
    const double u = pick(rng);
    double acc = 0;
    std::size_t c = 2;
    for (std::size_t t = 0; t < 3; ++t)
      if ((acc += weight[t]) >= u) {
        c = t;
        break;
      }
    std::normal_distribution<double> noise(0, sigma[c]);
    x.values.push_back(centers[c][0] + noise(rng));
    x.values.push_back(centers[c][1] + noise(rng));
  }
  return x;
}
// Component indices sorted by the first center dimension (fixture sanity only:
// kmeans seeding is identical for both paths, so order matching never needs
// it).
std::vector<std::size_t> centerOrder(const Model &m) {
  const std::size_t d = m.centers.dimensions;
  std::vector<std::size_t> idx(m.centers.rows());
  for (std::size_t i = 0; i < idx.size(); ++i)
    idx[i] = i;
  std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
    return m.centers.values[a * d] < m.centers.values[b * d];
  });
  return idx;
}
// Exact comparisons: QCOMPARE would route doubles through qFuzzyCompare,
// which would let any future accumulation-order drift slip through silently.
std::size_t firstDiff(const std::vector<double> &got,
                      const std::vector<double> &want) {
  for (std::size_t i = 0; i < std::min(got.size(), want.size()); ++i)
    if (!(got[i] == want[i]))
      return i;
  return std::min(got.size(), want.size());
}
void bitwiseEqual(const std::vector<double> &got,
                  const std::vector<double> &want, const char *what) {
  if (got == want)
    return;
  QFAIL((std::string(what) + ": divergence at index " +
         std::to_string(firstDiff(got, want)) + " (sizes " +
         std::to_string(got.size()) + " vs " + std::to_string(want.size()) +
         ")")
            .c_str());
}
void bitwiseEqual(double got, double want, const char *what) {
  char values[96];
  std::snprintf(values, sizeof(values), "%.17g vs %.17g", got, want);
  QVERIFY2(got == want, (std::string(what) + ": " + values).c_str());
}
void bitwiseEqual(const Result &got, const Result &want) {
  QVERIFY2(got.labels == want.labels, "labels diverge between paths");
  bitwiseEqual(got.objectiveHistory, want.objectiveHistory, "objectiveHistory");
  bitwiseEqual(got.confidence, want.confidence, "confidence");
  bitwiseEqual(got.squaredDistance, want.squaredDistance, "squaredDistance");
  bitwiseEqual(got.model.centers.values, want.model.centers.values, "centers");
  bitwiseEqual(got.model.variances, want.model.variances, "variances");
  bitwiseEqual(got.model.weights, want.model.weights, "weights");
  bitwiseEqual(got.bic, want.bic, "bic");
  QCOMPARE(got.iterations, want.iterations);
}
} // namespace
class TestGmmChunked : public QObject {
  Q_OBJECT
private slots:
  void consistency();
  void pathGate();
  void singleRowBlocks();
  void cancellationAndProgress();
  void rssBound();
};
void TestGmmChunked::consistency() {
  const Matrix x = mixedMixture(20000, 20261004);
  Options full;
  full.k = 3;
  full.tolerance = 1e-9; // tighten the gate to drive multi-iteration EM
  full.emChunkBudgetBytes = 0; // full-matrix path
  Options chunk = full;
  chunk.emChunkBudgetBytes = 8 * 1024; // force many blocks (n*k*8 > 8 KiB)
  const Result a = gmm(x, full), b = gmm(x, chunk);
  QVERIFY2(a.ok, a.error.c_str());
  QVERIFY2(b.ok, b.error.c_str());
  QVERIFY(!a.chunkedEm);
  QVERIFY(b.chunkedEm);
  QCOMPARE(a.model.centers.rows(), std::size_t(3));
  QCOMPARE(b.model.centers.rows(), std::size_t(3));
  QVERIFY2(a.iterations >= 3, "fixture must drive more than one EM iteration");
  // Bitwise equivalence: both paths accumulate every accumulator in the same
  // row-ascending order and kmeans seeding is identical, so nothing may drift.
  bitwiseEqual(a, b);
  // Guard: even a fixed fixture seed can land in a k-means local optimum
  // (observed ~3.3% of seeds, and a different libstdc++ normal_distribution
  // implementation can move the boundary). Such a run converges to split
  // clusters on BOTH paths (bitwise equal, so equivalence still holds), but
  // the recovery assertions below would then fail obscurely. Detect the
  // signature and name the cause: normal minimum centroid gap is 6.0 and the
  // final log-likelihood sits near -79.9k; a local optimum drops the gap to
  // well under 3 and the likelihood below -85k.
  double minCenterGapSq = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < 3; ++i)
    for (std::size_t j = i + 1; j < 3; ++j) {
      double gap = 0;
      for (std::size_t t = 0; t < 2; ++t) {
        const double delta = a.model.centers.values[i * 2 + t] -
                             a.model.centers.values[j * 2 + t];
        gap += delta * delta;
      }
      minCenterGapSq = std::min(minCenterGapSq, gap);
    }
  const bool localOptimum = std::sqrt(minCenterGapSq) < 3 ||
                            a.objectiveHistory.back() < -85e3;
  QVERIFY2(!localOptimum,
           "fixture seed hit a k-means local optimum; pick another seed");
  // Fixture sanity: the chunked run recovers the true components.
  const std::vector<std::size_t> order = centerOrder(b.model);
  const double truth[3][2] = {{-3, 3}, {0, -3}, {3, 3}};
  const double truthWeight[3] = {.2, .3, .5};
  for (std::size_t r = 0; r < order.size(); ++r) {
    const std::size_t c = order[r];
    for (std::size_t j = 0; j < 2; ++j)
      QVERIFY2(std::abs(b.model.centers.values[c * 2 + j] - truth[r][j]) < .15,
               "recovered center off the fixture component");
    QVERIFY2(std::abs(b.model.weights[c] - truthWeight[r]) < .05,
             "mixture weights off the fixture");
  }
  qInfo("consistency: full iterations=%d chunked iterations=%d", a.iterations,
        b.iterations);
}
void TestGmmChunked::pathGate() {
  const Matrix x = separableMixture(3000, 11);
  Options o;
  o.k = 3;
  o.emChunkBudgetBytes = 0;
  const Result full = gmm(x, o);
  QVERIFY2(full.ok, full.error.c_str());
  QVERIFY(!full.chunkedEm);
  o.emChunkBudgetBytes = 8 * 1024; // 3000*3*8 B = 72 KiB > 8 KiB
  const Result chunk = gmm(x, o);
  QVERIFY2(chunk.ok, chunk.error.c_str());
  QVERIFY(chunk.chunkedEm);
  // Small n with several blocks and a ragged last block; smoke-check the
  // final log-likelihood within 1e-9 relative.
  QVERIFY2(std::abs(chunk.objectiveHistory.back() -
                    full.objectiveHistory.back()) <=
               1e-9 * std::abs(full.objectiveHistory.back()),
           "small-n log-likelihood drift");
  QCOMPARE(chunk.labels.size(), full.labels.size());
  // n*k*8 within budget keeps the full-matrix path.
  o.emChunkBudgetBytes = 1 << 30;
  const Result wide = gmm(x, o);
  QVERIFY2(wide.ok, wide.error.c_str());
  QVERIFY(!wide.chunkedEm);
}
void TestGmmChunked::singleRowBlocks() {
  const Matrix x = mixedMixture(3000, 77);
  Options full;
  full.k = 3;
  full.tolerance = 1e-9;
  full.emChunkBudgetBytes = 0;
  Options chunk = full;
  chunk.emChunkBudgetBytes = 8; // chunkRows = max(1, 8/(3*8)) = 1
  const Result a = gmm(x, full), b = gmm(x, chunk);
  QVERIFY2(a.ok, a.error.c_str());
  QVERIFY2(b.ok, b.error.c_str());
  QVERIFY(b.chunkedEm);
  QVERIFY2(b.iterations >= 2, "fixture must drive more than one EM iteration");
  bitwiseEqual(a, b);
}
void TestGmmChunked::cancellationAndProgress() {
  const Matrix x = separableMixture(20000, 3);
  Options o;
  o.k = 3;
  o.emChunkBudgetBytes = 8 * 1024;
  // Progress only fires from inside the EM loop (kmeans seeding receives an
  // empty progress callback): cancel right after the first EM progress report
  // lands on a chunked pass-1 cancellation check for sure.
  bool inEm = false;
  const Control cancel{[&] { return inEm; },
                       [&](double p) {
                         QVERIFY(p > 0 && p <= 1);
                         inEm = true;
                       }};
  const Result r = gmm(x, o, cancel);
  QVERIFY(!r.ok);
  QVERIFY(r.cancelled);
  QVERIFY(r.labels.empty());
  double last = -1;
  int reports = 0;
  const Control steady{[&] { return false; },
                       [&](double p) {
                         QVERIFY(p >= last);
                         last = p;
                         ++reports;
                       }};
  const Result s = gmm(x, o, steady);
  QVERIFY2(s.ok, s.error.c_str());
  QVERIFY(s.chunkedEm);
  QCOMPARE(last, 1.);
  QVERIFY(reports >= 2); // at least one EM iteration report plus completion
}
void TestGmmChunked::rssBound() {
#if defined(Q_OS_WIN)
  QSKIP("VmHWM peak RSS bound is measured on Linux");
#else
  const double ms = 1e9;
  QElapsedTimer timer;
  timer.start();
  // n=4M, d=2, k=4: input body 64MB, full-matrix responsibilities 128MB.
  const std::size_t n = 4000000, k = 4;
  Matrix x{2, {}};
  x.values.reserve(n * 2);
  std::mt19937_64 rng(97);
  const double centers[4][2] = {{-12, 9}, {12, 9}, {-12, -9}, {12, -9}};
  std::normal_distribution<double> noise(0, 1);
  std::uniform_int_distribution<int> pick(0, 3);
  for (std::size_t i = 0; i < n; ++i) {
    const double *c = centers[pick(rng)];
    x.values.push_back(c[0] + noise(rng));
    x.values.push_back(c[1] + noise(rng));
  }
  Options o;
  o.k = int(k);
  o.maxIterations = 50; // time discipline: two-pass EM converges in budget
  o.tolerance = 1e-4;   // relax the convergence gate for a memory-only run
  o.emChunkBudgetBytes = 0;
  Options chunk = o;
  chunk.emChunkBudgetBytes = 8 * 1024 * 1024;
  trimHeap();
  const qint64 base = peakRssKb();
  QVERIFY2(base > 0, "VmHWM not readable from /proc/self/status");
  // Chunked runs first: VmHWM is a high-water mark that never falls, so the
  // full run afterwards stacks its 128MB responsibility buffer on top.
  const Result a = gmm(x, chunk);
  const qint64 peakA = peakRssKb();
  const Result b = gmm(x, o);
  const qint64 peakB = peakRssKb();
  QVERIFY2(a.ok, a.error.c_str());
  QVERIFY2(b.ok, b.error.c_str());
  QVERIFY(a.chunkedEm);
  QVERIFY(!b.chunkedEm);
  const double inputKb = double(x.values.size() * sizeof(double)) / 1024.0;
  const double budgetKb = double(8 * 1024 * 1024) / 1024.0;
  const double avoidedKb = 0.5 * double(n * k * 8) / 1024.0;
  const double aKb = double(peakA - base), bKb = double(peakB - base);
  qInfo("GMM RSS base=%.1f MiB chunked=%.1f MiB full=%.1f MiB (%.1f s, "
        "iterations full=%d chunked=%d)",
        double(base) / 1024.0, aKb / 1024.0, bKb / 1024.0,
        double(timer.nsecsElapsed()) / ms, b.iterations, a.iterations);
  QVERIFY2(bKb >= aKb + avoidedKb,
           "chunked EM did not avoid the n*k responsibility buffer");
  // Tight upper bound: a chunked run that quietly materialized the full n*k
  // buffer would land near the full-path peak (~320 MiB) and fail here.
  QVERIFY2(aKb <= inputKb * 2 + budgetKb * 2 + 64.0 * 1024.0,
           "chunked EM peak above the budgeted bound");
#endif
}
QTEST_APPLESS_MAIN(TestGmmChunked)
#include "tst_gmm_chunked.moc"
