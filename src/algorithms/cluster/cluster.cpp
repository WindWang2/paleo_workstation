// 层：数据
#include "cluster.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>

namespace paleo::cluster {
namespace {
constexpr double pi = 3.14159265358979323846;
bool stopped(const Control &c) { return c.cancelled && c.cancelled(); }
void report(const Control &c, double p) {
  if (c.progress)
    c.progress(p);
}
Result failure(const std::string &e, bool cancel = false) {
  Result r;
  r.error = e;
  r.cancelled = cancel;
  return r;
}
bool valid(const Matrix &x) {
  return x.dimensions > 0 && x.values.size() % x.dimensions == 0 &&
         std::all_of(x.values.begin(), x.values.end(),
                     [](double v) { return std::isfinite(v); });
}
bool validOptions(const Options &o) {
  return o.k > 0 && o.maxIterations > 0 && std::isfinite(o.tolerance) &&
         o.tolerance >= 0 && std::isfinite(o.varianceFloor) &&
         o.varianceFloor > 0;
}
double distance(const double *a, const double *b, std::size_t d) {
  double v = 0;
  for (std::size_t j = 0; j < d; ++j) {
    const double t = a[j] - b[j];
    v += t * t;
  }
  return v;
}
// Log-sum-exp avoids Gaussian underflow for distant samples.
double posterior(const double *x, const Model &m, std::vector<double> &p) {
  const std::size_t d = m.centers.dimensions, k = m.centers.rows();
  p.resize(k);
  for (std::size_t c = 0; c < k; ++c) {
    double v = std::log(m.weights[c]);
    for (std::size_t j = 0; j < d; ++j) {
      const double var = m.variances[c * d + j],
                   delta = x[j] - m.centers.values[c * d + j];
      v -= 0.5 * (std::log(2 * pi * var) + delta * delta / var);
    }
    p[c] = v;
  }
  const double max = *std::max_element(p.begin(), p.end());
  double sum = 0;
  for (double &v : p) {
    v = std::exp(v - max);
    sum += v;
  }
  for (double &v : p)
    v /= sum;
  return max + std::log(sum);
}
} // namespace

Result predict(const Matrix &x, const Model &m, const Control &ctl) {
  if (!valid(x) || !valid(m.centers) || m.centers.dimensions != x.dimensions ||
      (x.rows() && !m.centers.rows()))
    return failure("Invalid matrix/model dimensions or non-finite values");
  const auto n = x.rows(), d = x.dimensions, k = m.centers.rows();
  if (m.method == Method::Gmm &&
      (m.variances.size() != k * d || m.weights.size() != k ||
       !std::all_of(m.variances.begin(), m.variances.end(),
                    [](double v) { return std::isfinite(v) && v > 0; }) ||
       !std::all_of(m.weights.begin(), m.weights.end(),
                    [](double v) { return std::isfinite(v) && v > 0; })))
    return failure("Invalid Gaussian parameters");
  Result r;
  r.model = m;
  r.labels.resize(n);
  r.confidence.resize(n);
  r.squaredDistance.resize(n);
  std::vector<double> p;
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 1023U) == 0) {
      if (stopped(ctl))
        return failure("Cancelled", true);
      report(ctl, double(i) / n);
    }
    std::size_t winner = 0;
    double best = std::numeric_limits<double>::infinity(), second = best;
    for (std::size_t c = 0; c < k; ++c) {
      const double v = distance(x.row(i), m.centers.row(c), d);
      if (v < best) {
        second = best;
        best = v;
        winner = c;
      } else if (v < second)
        second = v;
    }
    if (m.method == Method::Gmm) {
      if (!std::isfinite(posterior(x.row(i), m, p)))
        return failure("Non-finite Gaussian posterior");
      winner = std::size_t(std::max_element(p.begin(), p.end()) - p.begin());
      r.confidence[i] = p[winner];
      best = distance(x.row(i), m.centers.row(winner), d);
    } else
      r.confidence[i] =
          k == 1 ? 1
                 : (second > 0 ? std::clamp(1 - best / second, 0.0, 1.0) : 0);
    if (!std::isfinite(best))
      return failure("Numerical overflow in distance");
    r.labels[i] = int(winner);
    r.squaredDistance[i] = best;
  }
  if (stopped(ctl))
    return failure("Cancelled", true);
  r.ok = true;
  report(ctl, 1);
  return r;
}

Result kmeans(const Matrix &x, const Options &o, const Control &ctl) {
  if (!valid(x) || !validOptions(o))
    return failure("Invalid samples or clustering parameters");
  if (stopped(ctl))
    return failure("Cancelled", true);
  const auto n = x.rows(), d = x.dimensions;
  Model m;
  m.centers.dimensions = d;
  if (!n) {
    Result r;
    r.ok = true;
    r.model = m;
    report(ctl, 1);
    return r;
  }
  std::mt19937_64 rng(o.seed);
  const auto first = std::uniform_int_distribution<std::size_t>(0, n - 1)(rng);
  m.centers.values.assign(x.row(first), x.row(first) + d);
  std::vector<double> nearest(n, std::numeric_limits<double>::infinity());
  const std::size_t requested = std::min(std::size_t(o.k), n);
  for (std::size_t c = 1; c < requested; ++c) {
    double sum = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if ((i & 1023U) == 0 && stopped(ctl))
        return failure("Cancelled", true);
      nearest[i] =
          std::min(nearest[i], distance(x.row(i), m.centers.row(c - 1), d));
      sum += nearest[i];
    }
    if (!std::isfinite(sum))
      return failure("Numerical overflow in kmeans++");
    if (sum == 0)
      break; // fewer distinct samples than k; report the effective k
    const double target = std::uniform_real_distribution<double>(0, sum)(rng);
    double cumulative = 0;
    std::size_t picked = n - 1;
    for (std::size_t i = 0; i < n; ++i) {
      cumulative += nearest[i];
      if (cumulative > target) {
        picked = i;
        break;
      }
    }
    m.centers.values.insert(m.centers.values.end(), x.row(picked),
                            x.row(picked) + d);
  }
  const auto k = m.centers.rows();
  std::vector<int> labels(n, -1);
  std::vector<double> sums(k * d);
  std::vector<std::size_t> counts(k);
  std::vector<double> history;
  int iterations = 0;
  for (int it = 0; it < o.maxIterations; ++it) {
    std::fill(sums.begin(), sums.end(), 0);
    std::fill(counts.begin(), counts.end(), 0);
    bool changed = false;
    double sse = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if ((i & 1023U) == 0 && stopped(ctl))
        return failure("Cancelled", true);
      double best = std::numeric_limits<double>::infinity();
      int winner = 0;
      for (std::size_t c = 0; c < k; ++c) {
        const double v = distance(x.row(i), m.centers.row(c), d);
        if (v < best) {
          best = v;
          winner = int(c);
        }
      }
      changed = changed || labels[i] != winner;
      labels[i] = winner;
      sse += best;
      ++counts[std::size_t(winner)];
      for (std::size_t j = 0; j < d; ++j)
        sums[std::size_t(winner) * d + j] += x.row(i)[j];
    }
    if (!std::isfinite(sse))
      return failure("Numerical overflow in kmeans");
    history.push_back(sse);
    double shift = 0;
    for (std::size_t c = 0; c < k; ++c)
      if (counts[c]) {
        for (std::size_t j = 0; j < d; ++j) {
          const double v = sums[c * d + j] / double(counts[c]);
          const double delta = v - m.centers.values[c * d + j];
          shift += delta * delta;
          m.centers.values[c * d + j] = v;
        }
      }
    iterations = it + 1;
    report(ctl, 0.9 * double(iterations) / o.maxIterations);
    if (!changed || shift <= o.tolerance * o.tolerance)
      break;
  }
  Control final{ctl.cancelled, {}};
  Result r = predict(x, m, final);
  if (!r.ok)
    return r;
  m.weights.assign(k, 0);
  for (int l : r.labels)
    m.weights[std::size_t(l)] += 1.0 / double(n);
  r.model = m;
  r.iterations = iterations;
  r.objectiveHistory = history;
  r.objectiveHistory.push_back(
      std::accumulate(r.squaredDistance.begin(), r.squaredDistance.end(), 0.0));
  report(ctl, 1);
  return r;
}

Result gmm(const Matrix &x, const Options &o, const Control &ctl) {
  if (!valid(x) || !validOptions(o))
    return failure("Invalid samples or clustering parameters");
  Control initCtl{ctl.cancelled, {}};
  auto init = kmeans(x, o, initCtl);
  if (!init.ok)
    return init;
  Model m = init.model;
  m.method = Method::Gmm;
  const auto n = x.rows(), d = x.dimensions, k = m.centers.rows();
  if (!n) {
    init.model = m;
    init.objectiveHistory.clear();
    report(ctl, 1);
    return init;
  }
  m.variances.assign(k * d, o.varianceFloor);
  std::vector<double> sums(k * d, 0), counts(k, 0), p;
  for (std::size_t i = 0; i < n; ++i) {
    const auto c = std::size_t(init.labels[i]);
    ++counts[c];
    for (std::size_t j = 0; j < d; ++j) {
      const double delta = x.row(i)[j] - m.centers.values[c * d + j];
      sums[c * d + j] += delta * delta;
    }
  }
  for (std::size_t c = 0; c < k; ++c) {
    m.weights[c] = std::max(counts[c] / double(n), 1e-12);
    for (std::size_t j = 0; j < d; ++j)
      m.variances[c * d + j] = std::max(
          counts[c] ? sums[c * d + j] / counts[c] : 0, o.varianceFloor);
  }
  const double weightSum =
      std::accumulate(m.weights.begin(), m.weights.end(), 0.0);
  for (double &w : m.weights)
    w /= weightSum;
  std::vector<double> responsibilities(n * k), history;
  int iterations = 0;
  for (int it = 0; it <= o.maxIterations; ++it) {
    double ll = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if ((i & 1023U) == 0 && stopped(ctl))
        return failure("Cancelled", true);
      ll += posterior(x.row(i), m, p);
      std::copy(p.begin(), p.end(),
                responsibilities.begin() + std::ptrdiff_t(i * k));
    }
    if (!std::isfinite(ll))
      return failure("Non-finite Gaussian likelihood");
    history.push_back(ll);
    if (it &&
        ll + 1e-8 * std::max(1.0, std::abs(ll)) < history[history.size() - 2])
      return failure("Gaussian EM likelihood decreased; model not accepted");
    if (it == o.maxIterations ||
        (it && std::abs(ll - history[history.size() - 2]) <=
                   o.tolerance * std::max(1.0, std::abs(ll))))
      break;
    std::fill(counts.begin(), counts.end(), 0);
    std::fill(sums.begin(), sums.end(), 0);
    for (std::size_t i = 0; i < n; ++i) {
      if ((i & 1023U) == 0 && stopped(ctl))
        return failure("Cancelled", true);
      for (std::size_t c = 0; c < k; ++c) {
        const double w = responsibilities[i * k + c];
        counts[c] += w;
        for (std::size_t j = 0; j < d; ++j)
          sums[c * d + j] += w * x.row(i)[j];
      }
    }
    for (std::size_t c = 0; c < k; ++c)
      if (counts[c] > 1e-12) {
        m.weights[c] = counts[c] / double(n);
        for (std::size_t j = 0; j < d; ++j)
          m.centers.values[c * d + j] = sums[c * d + j] / counts[c];
      }
    std::fill(sums.begin(), sums.end(), 0);
    for (std::size_t i = 0; i < n; ++i) {
      if ((i & 1023U) == 0 && stopped(ctl))
        return failure("Cancelled", true);
      for (std::size_t c = 0; c < k; ++c)
        for (std::size_t j = 0; j < d; ++j) {
          const double delta = x.row(i)[j] - m.centers.values[c * d + j];
          sums[c * d + j] += responsibilities[i * k + c] * delta * delta;
        }
    }
    for (std::size_t c = 0; c < k; ++c)
      if (counts[c] > 1e-12)
        for (std::size_t j = 0; j < d; ++j)
          m.variances[c * d + j] =
              std::max(sums[c * d + j] / counts[c], o.varianceFloor);
    iterations = it + 1;
    report(ctl, 0.9 * double(iterations) / o.maxIterations);
  }
  Result r = predict(x, m, initCtl);
  if (!r.ok)
    return r;
  r.objectiveHistory = history;
  r.iterations = iterations;
  r.bic = double(2 * k * d + k - 1) * std::log(double(n)) - 2 * history.back();
  report(ctl, 1);
  return r;
}

namespace {
double cross(Point a, Point b, Point c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
bool onSegment(Point p, Point a, Point b) {
  const double scale =
      std::max({1.0, std::abs(b.x - a.x), std::abs(b.y - a.y)});
  const double eps = 1e-10 * scale;
  return std::abs(cross(a, b, p)) <= eps * scale &&
         p.x >= std::min(a.x, b.x) - eps && p.x <= std::max(a.x, b.x) + eps &&
         p.y >= std::min(a.y, b.y) - eps && p.y <= std::max(a.y, b.y) + eps;
}
} // namespace
std::vector<Point> convexHull(std::vector<Point> p) {
  p.erase(std::remove_if(p.begin(), p.end(),
                         [](Point v) {
                           return !std::isfinite(v.x) || !std::isfinite(v.y);
                         }),
          p.end());
  std::sort(p.begin(), p.end(), [](Point a, Point b) {
    return a.x < b.x || (a.x == b.x && a.y < b.y);
  });
  p.erase(
      std::unique(p.begin(), p.end(),
                  [](Point a, Point b) { return a.x == b.x && a.y == b.y; }),
      p.end());
  if (p.size() < 3)
    return p;
  std::vector<Point> hull(2 * p.size());
  std::size_t k = 0;
  for (Point v : p) {
    while (k >= 2 && cross(hull[k - 2], hull[k - 1], v) <= 0)
      --k;
    hull[k++] = v;
  }
  const auto base = k + 1;
  for (std::size_t i = p.size() - 1; i-- > 0;) {
    while (k >= base && cross(hull[k - 2], hull[k - 1], p[i]) <= 0)
      --k;
    hull[k++] = p[i];
  }
  hull.resize(k - 1);
  return hull;
}
bool inPolygon(Point p, const std::vector<Point> &v) {
  if (v.empty() || !std::isfinite(p.x) || !std::isfinite(p.y))
    return false;
  bool inside = false;
  for (std::size_t i = 0, j = v.size() - 1; i < v.size(); j = i++) {
    if (onSegment(p, v[j], v[i]))
      return true;
    if ((v[i].y > p.y) != (v[j].y > p.y) &&
        p.x < (v[j].x - v[i].x) * (p.y - v[i].y) / (v[j].y - v[i].y) + v[i].x)
      inside = !inside;
  }
  return inside;
}
bool inBox(const double *p, const std::vector<double> &lo,
           const std::vector<double> &hi) {
  if (!p || lo.empty() || lo.size() != hi.size())
    return false;
  for (std::size_t j = 0; j < lo.size(); ++j)
    if (!std::isfinite(p[j]) || !std::isfinite(lo[j]) ||
        !std::isfinite(hi[j]) || lo[j] > hi[j] || p[j] < lo[j] || p[j] > hi[j])
      return false;
  return true;
}
} // namespace paleo::cluster
