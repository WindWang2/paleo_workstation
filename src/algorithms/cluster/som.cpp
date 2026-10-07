// 层：数据
#include "som.h"
#include "cluster_internal.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>

namespace paleo::cluster {
namespace {
SomResult somFailure(const std::string &e, bool cancel = false) {
  SomResult r;
  r.error = e;
  r.cancelled = cancel;
  return r;
}
bool valid(const Matrix &x) {
  return x.dimensions > 0 && x.values.size() % x.dimensions == 0 &&
         std::all_of(x.values.begin(), x.values.end(),
                     [](double v) { return std::isfinite(v); });
}
// Grids beyond this cap cannot train in bounded memory; reject up front
// rather than overflowing the cell product or throwing out of the somFailure
// path — every rejection must travel back as SomResult::error.
constexpr std::size_t maxGridCells = 1'000'000;
constexpr std::size_t maxPrototypeValues = 100'000'000;
bool validOptions(const SomOptions &o) {
  // width/height are validated positive, so the size_t cell product of two
  // 31-bit values cannot overflow.
  const std::size_t cells = std::size_t(o.width) * std::size_t(o.height);
  return o.width > 0 && o.height > 0 && o.epochs > 0 &&
         cells <= maxGridCells && std::isfinite(o.initialLearningRate) &&
         o.initialLearningRate > 0 && o.initialLearningRate <= 1 &&
         std::isfinite(o.finalLearningRate) && o.finalLearningRate > 0 &&
         o.finalLearningRate <= 1 && std::isfinite(o.initialRadius) &&
         o.initialRadius >= 0 && std::isfinite(o.finalRadius) &&
         o.finalRadius >= 0;
}
double distance(const double *a, const double *b, std::size_t d) {
  double v = 0;
  for (std::size_t j = 0; j < d; ++j) {
    const double t = a[j] - b[j];
    v += t * t;
  }
  return v;
}
// Ties resolve to the lower prototype index.
void nearest(const Matrix &protos, const double *sample, std::size_t &winner,
             double &best, double &second) {
  const auto k = protos.rows(), d = protos.dimensions;
  winner = 0;
  best = std::numeric_limits<double>::infinity();
  second = best;
  for (std::size_t c = 0; c < k; ++c) {
    const double v = distance(sample, protos.row(c), d);
    if (v < best) {
      second = best;
      best = v;
      winner = c;
    } else if (v < second)
      second = v;
  }
}
} // namespace

SomResult som(const Matrix &x, const SomOptions &o, const Control &ctl) {
  if (!valid(x) || !validOptions(o))
    return somFailure("Invalid samples or SOM parameters");
  if (stopped(ctl))
    return somFailure("Cancelled", true);
  const auto n = x.rows(), d = x.dimensions;
  if (!n)
    return somFailure("Empty sample matrix");
  const auto units = std::size_t(o.width) * std::size_t(o.height);
  // Keep the prototype buffer inside the memory budget; a bad_alloc here
  // would escape the "failures come back as SomResult" convention.
  if (units > maxPrototypeValues / d)
    return somFailure("Prototype grid too large for the sample dimension");
  SomResult r;
  r.width = o.width;
  r.height = o.height;
  r.prototypes.dimensions = d;
  r.prototypes.values.assign(units * d, 0.0);
  std::mt19937_64 rng(o.seed);
  std::vector<std::size_t> sequence(n);
  std::iota(sequence.begin(), sequence.end(), std::size_t(0));
  std::shuffle(sequence.begin(), sequence.end(), rng);
  for (std::size_t u = 0; u < units; ++u) {
    // Seeded rows without replacement; once the grid outgrows the sample
    // count the remaining prototypes repeat draws with replacement, so every
    // prototype stays a finite copy of a real sample.
    const std::size_t pick =
        u < n ? sequence[u]
              : std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
    std::copy(x.row(pick), x.row(pick) + d,
              r.prototypes.values.begin() +
                  std::ptrdiff_t(u) * std::ptrdiff_t(d));
  }
  std::vector<int> gridX(units), gridY(units);
  for (std::size_t u = 0; u < units; ++u) {
    gridX[u] = int(u % std::size_t(o.width));
    gridY[u] = int(u / std::size_t(o.width));
  }
  const double lr0 = o.initialLearningRate, lr1 = o.finalLearningRate;
  const double r0 =
      o.initialRadius > 0 ? o.initialRadius : std::max(o.width, o.height) / 2.0;
  const double r1 = std::max(o.finalRadius, 0.0);
  std::vector<int> labels(n);
  std::vector<double> confidence(n), squared(n);
  for (int e = 0; e < o.epochs; ++e) {
    const double t = double(e) / double(o.epochs);
    const double eta = lr0 * std::pow(lr1 / lr0, t);
    // Floor keeps the Gaussian denominator finite when the radius decays to
    // zero; exp(-inf) then collapses influence onto the BMU itself.
    const double radius = std::max(r0 * std::pow(r1 / r0, t), 1e-12);
    const double denom = 2 * radius * radius;
    std::shuffle(sequence.begin(), sequence.end(), rng);
    for (std::size_t s = 0; s < n; ++s) {
      if ((s & 1023U) == 0 && stopped(ctl))
        return somFailure("Cancelled", true);
      const double *sample = x.row(sequence[s]);
      std::size_t winner = 0;
      double best = 0, second = 0;
      nearest(r.prototypes, sample, winner, best, second);
      const int bx = gridX[winner], by = gridY[winner];
      for (std::size_t u = 0; u < units; ++u) {
        const double dx = gridX[u] - bx;
        const double dy = gridY[u] - by;
        const double h = std::exp(-(dx * dx + dy * dy) / denom);
        const double pull = eta * h;
        double *proto =
            r.prototypes.values.data() + std::ptrdiff_t(u) * std::ptrdiff_t(d);
        for (std::size_t j = 0; j < d; ++j)
          proto[j] += pull * (sample[j] - proto[j]);
      }
    }
    double total = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if ((i & 1023U) == 0 && stopped(ctl))
        return somFailure("Cancelled", true);
      std::size_t winner = 0;
      double best = 0, second = 0;
      nearest(r.prototypes, x.row(i), winner, best, second);
      if (!std::isfinite(best))
        return somFailure("Numerical overflow in distance");
      labels[i] = int(winner);
      squared[i] = best;
      total += best;
      confidence[i] =
          units == 1 ? 1
                     : (second > 0 ? std::clamp(1 - best / second, 0.0, 1.0)
                                   : 0);
    }
    r.quantizationErrorHistory.push_back(total / double(n));
    report(ctl, 0.9 * double(e + 1) / double(o.epochs));
  }
  if (stopped(ctl))
    return somFailure("Cancelled", true);
  r.labels = std::move(labels);
  r.confidence = std::move(confidence);
  r.squaredDistance = std::move(squared);
  r.ok = true;
  report(ctl, 1);
  return r;
}
} // namespace paleo::cluster
