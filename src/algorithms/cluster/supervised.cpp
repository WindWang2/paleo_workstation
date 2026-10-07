// 层：数据
#include "supervised.h"
#include "cluster_internal.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <utility>

namespace paleo::cluster {
namespace {
constexpr double pi = 3.14159265358979323846;
SupervisedResult supervisedFailure(const std::string &e, bool cancel = false) {
  SupervisedResult r;
  r.error = e;
  r.cancelled = cancel;
  return r;
}
CrossValidationResult cvFailure(const std::string &e, bool cancel = false) {
  CrossValidationResult r;
  r.error = e;
  r.cancelled = cancel;
  return r;
}
bool validMatrix(const Matrix &x) {
  return x.dimensions > 0 && x.values.size() % x.dimensions == 0 &&
         std::all_of(x.values.begin(), x.values.end(),
                     [](double v) { return std::isfinite(v); });
}
bool validOptions(const SupervisedOptions &o) {
  if (!std::isfinite(o.varianceFloor) || o.varianceFloor <= 0)
    return false;
  return o.method != SupervisedMethod::Knn || o.knnNeighbors >= 1;
}
bool consistentModel(const SupervisedModel &m, std::size_t d) {
  const std::size_t c = m.classIds.size();
  if (!c || m.priors.size() != c)
    return false;
  if (!std::all_of(m.priors.begin(), m.priors.end(), [](double p) {
        return std::isfinite(p) && p > 0;
      }))
    return false;
  if (m.method == SupervisedMethod::Knn) {
    if (m.knnNeighbors < 1 || !m.trainValues.rows() ||
        m.trainValues.dimensions != d)
      return false;
    return m.trainValues.rows() == m.trainLabels.size() &&
           std::all_of(m.trainLabels.begin(), m.trainLabels.end(),
                       [c](int l) { return l >= 0 && std::size_t(l) < c; });
  }
  if (m.means.dimensions != d || m.means.rows() != c)
    return false;
  const std::size_t want =
      m.method == SupervisedMethod::Lda ? d * d : c * d * d;
  return m.covariances.size() == want &&
         std::all_of(m.covariances.begin(), m.covariances.end(),
                     [](double v) { return std::isfinite(v); });
}
double squared(const double *a, const double *b, std::size_t d) {
  double v = 0;
  for (std::size_t j = 0; j < d; ++j) {
    const double t = a[j] - b[j];
    v += t * t;
  }
  return v;
}
// Lower-triangular Cholesky factor L with L * L^T = a; false on non-positive
// pivot.
bool cholesky(const std::vector<double> &a, std::size_t d,
              std::vector<double> &l) {
  l.assign(d * d, 0);
  for (std::size_t i = 0; i < d; ++i)
    for (std::size_t j = 0; j <= i; ++j) {
      double s = a[i * d + j];
      for (std::size_t k = 0; k < j; ++k)
        s -= l[i * d + k] * l[j * d + k];
      if (i == j) {
        if (!(s > 0))
          return false;
        l[i * d + i] = std::sqrt(s);
      } else
        l[i * d + j] = s / l[j * d + j];
    }
  return true;
}
// Replaces y with the solution of L * L^T * y = b given factor L.
void choleskySolve(const double *l, std::size_t d, double *y) {
  for (std::size_t i = 0; i < d; ++i) {
    double s = y[i];
    for (std::size_t k = 0; k < i; ++k)
      s -= l[i * d + k] * y[k];
    y[i] = s / l[i * d + i];
  }
  for (std::size_t i = d; i-- > 0;) {
    double s = y[i];
    for (std::size_t k = i + 1; k < d; ++k)
      s -= l[k * d + i] * y[k];
    y[i] = s / l[i * d + i];
  }
}
// Regularizes a scatter matrix with a variance floor on the diagonal and
// factors it; a failed factorization retries with the floor scaled by ten,
// three times at most, before giving up. The regularized scatter is handed
// back for the model, the log determinant serves the classifier.
bool factorScatter(const std::vector<double> &scatter, std::size_t d,
                   double floor, std::vector<double> &covariance,
                   double &logDet) {
  covariance = scatter;
  for (int attempt = 0; attempt <= 3; ++attempt) {
    const double ridge = floor * std::pow(10.0, attempt);
    for (std::size_t i = 0; i < d; ++i)
      covariance[i * d + i] = scatter[i * d + i] + ridge;
    std::vector<double> factor;
    if (!cholesky(covariance, d, factor))
      continue;
    logDet = 0;
    for (std::size_t i = 0; i < d; ++i)
      logDet += 2 * std::log(factor[i * d + i]);
    if (std::isfinite(logDet))
      return true;
  }
  return false;
}
struct Labeled {
  std::vector<std::size_t> rows; // original row indices, ascending
  std::vector<int> classIds;     // sorted unique codes
  std::vector<int> rowClass;     // index into classIds, parallel to rows
};
Labeled labeled(const Matrix &x, const std::vector<int> &labels) {
  Labeled out;
  for (std::size_t i = 0; i < x.rows(); ++i)
    if (labels[i] != -1) {
      out.rows.push_back(i);
      out.classIds.push_back(labels[i]);
    }
  std::sort(out.classIds.begin(), out.classIds.end());
  out.classIds.erase(std::unique(out.classIds.begin(), out.classIds.end()),
                     out.classIds.end());
  out.rowClass.reserve(out.rows.size());
  for (std::size_t i = 0; i < out.rows.size(); ++i)
    out.rowClass.push_back(
        int(std::lower_bound(out.classIds.begin(), out.classIds.end(),
                             labels[out.rows[i]]) -
            out.classIds.begin()));
  return out;
}
// Log-posterior argmax with log-sum-exp normalization. Ties keep the smaller
// classIds index. Returns false on non-finite scores. The score/delta/
// mahalanobis buffers are caller-owned so one classification pass allocates
// them once; delta is overwritten in place by the triangular solve, so the
// squared distance reuses the original center difference.
bool classifyGaussian(const double *x, const SupervisedModel &m,
                      const std::vector<double> &factors,
                      const std::vector<double> &logDets,
                      std::vector<double> &score, std::vector<double> &delta,
                      std::vector<double> &mahalanobis, int &label,
                      double &confidence, double &squaredDistance) {
  const std::size_t c = m.classIds.size(), d = m.dimensions;
  const std::size_t stride = m.method == SupervisedMethod::Lda ? 0 : d * d;
  score.resize(c);
  delta.resize(d);
  mahalanobis.resize(c);
  for (std::size_t k = 0; k < c; ++k) {
    for (std::size_t j = 0; j < d; ++j)
      delta[j] = x[j] - m.means.values[k * d + j];
    choleskySolve(&factors[k * stride], d, delta.data());
    double m2 = 0;
    for (std::size_t j = 0; j < d; ++j)
      m2 += (x[j] - m.means.values[k * d + j]) * delta[j];
    mahalanobis[k] = m2;
    score[k] = std::log(m.priors[k]) -
               0.5 * (double(d) * std::log(2 * pi) + logDets[k] + m2);
    if (!std::isfinite(score[k]))
      return false;
  }
  const double top = *std::max_element(score.begin(), score.end());
  double sum = 0;
  for (double &s : score) {
    s = std::exp(s - top);
    sum += s;
  }
  std::size_t winner = 0;
  for (std::size_t k = 0; k < c; ++k) {
    score[k] /= sum;
    if (score[k] > score[winner])
      winner = k;
  }
  label = int(winner);
  confidence = std::clamp(score[winner], 0.0, 1.0);
  squaredDistance = mahalanobis[winner];
  return true;
}
// kNN vote; neighbor and vote ties keep the smaller index. The order/votes
// buffers are caller-owned so one classification pass allocates them once.
void classifyKnn(const double *x, const SupervisedModel &m,
                 std::vector<std::pair<double, std::size_t>> &order,
                 std::vector<std::size_t> &votes, int &label,
                 double &confidence, double &meanDistance) {
  const std::size_t c = m.classIds.size(), n = m.trainValues.rows(),
                    d = m.dimensions;
  order.resize(n);
  for (std::size_t i = 0; i < n; ++i)
    order[i] = {squared(x, m.trainValues.row(i), d), i};
  const std::size_t k =
      std::min<std::size_t>(std::size_t(m.knnNeighbors), n);
  std::partial_sort(order.begin(), order.begin() + std::ptrdiff_t(k),
                    order.end(), [](const auto &a, const auto &b) {
                      return a.first < b.first ||
                             (a.first == b.first && a.second < b.second);
                    });
  votes.assign(c, 0);
  double sum = 0;
  for (std::size_t j = 0; j < k; ++j) {
    votes[std::size_t(m.trainLabels[order[j].second])] += 1;
    sum += order[j].first;
  }
  std::size_t winner = 0;
  for (std::size_t t = 1; t < c; ++t)
    if (votes[t] > votes[winner])
      winner = t;
  label = int(winner);
  confidence = std::clamp(double(votes[winner]) / double(k), 0.0, 1.0);
  meanDistance = sum / double(k);
}
// Shared classifier for train and predict; identical inputs produce
// bit-identical outputs, which the in-sample fit relies on. Progress and
// cancellation follow the (i & 1023U)==0 rhythm; on supervisedFailure r carries the
// reason and r.ok stays false.
bool classifyRows(const Matrix &x, const SupervisedModel &m, const Control &ctl,
                  double from, SupervisedResult &r) {
  const auto n = x.rows();
  std::vector<double> factors, logDets;
  if (m.method != SupervisedMethod::Knn) {
    const std::size_t c = m.classIds.size(), d = m.dimensions;
    const std::size_t blocks = m.method == SupervisedMethod::Lda ? 1 : c;
    factors.assign(blocks * d * d, 0);
    logDets.assign(c, 0);
    for (std::size_t b = 0; b < blocks; ++b) {
      std::vector<double> block(m.covariances.begin() +
                                    std::ptrdiff_t(b) * std::ptrdiff_t(d * d),
                                m.covariances.begin() +
                                    std::ptrdiff_t(b + 1) *
                                        std::ptrdiff_t(d * d));
      std::vector<double> factor;
      double logDet = 0;
      if (!cholesky(block, d, factor)) {
        r.error = "Covariance matrix is not positive definite";
        return false;
      }
      for (std::size_t i = 0; i < d; ++i)
        logDet += 2 * std::log(factor[i * d + i]);
      if (!std::isfinite(logDet)) {
        r.error = "Covariance matrix is not positive definite";
        return false;
      }
      std::copy(factor.begin(), factor.end(),
                factors.begin() + std::ptrdiff_t(b) * std::ptrdiff_t(d * d));
      if (m.method == SupervisedMethod::Lda)
        for (std::size_t k = 0; k < c; ++k)
          logDets[k] = logDet;
      else
        logDets[b] = logDet;
    }
  }
  r.labels.resize(n);
  r.confidence.resize(n);
  r.squaredDistance.resize(n);
  std::vector<std::pair<double, std::size_t>> order;
  std::vector<std::size_t> votes;
  std::vector<double> score, delta, mahalanobis;
  for (std::size_t i = 0; i < n; ++i) {
    if ((i & 1023U) == 0) {
      if (stopped(ctl)) {
        r.cancelled = true;
        r.error = "Cancelled";
        return false;
      }
      report(ctl, from + (1 - from) * double(i) / double(n));
    }
    if (m.method == SupervisedMethod::Knn)
      classifyKnn(x.row(i), m, order, votes, r.labels[i], r.confidence[i],
                  r.squaredDistance[i]);
    else if (!classifyGaussian(x.row(i), m, factors, logDets, score, delta,
                               mahalanobis, r.labels[i], r.confidence[i],
                               r.squaredDistance[i])) {
      r.error = "Non-finite classification scores";
      return false;
    }
    if (!std::isfinite(r.confidence[i]) ||
        !std::isfinite(r.squaredDistance[i])) {
      r.error = "Non-finite classification scores";
      return false;
    }
  }
  return true;
}
SupervisedResult classified(const Matrix &x, const SupervisedModel &m,
                            const Control &ctl, double from) {
  SupervisedResult r;
  r.model = m;
  if (!classifyRows(x, m, ctl, from, r)) {
    const bool cancelled = r.cancelled;
    const std::string error = std::move(r.error);
    r = SupervisedResult{};
    r.error = error;
    r.cancelled = cancelled;
    return r;
  }
  r.ok = true;
  return r;
}
} // namespace

SupervisedResult trainSupervised(const Matrix &x,
                                 const std::vector<int> &labels,
                                 const SupervisedOptions &o,
                                 const Control &ctl) {
  if (!validMatrix(x))
    return supervisedFailure("Non-finite sample values or zero dimensions");
  if (labels.size() != x.rows())
    return supervisedFailure("Label count does not match sample matrix rows");
  if (!validOptions(o))
    return supervisedFailure("Invalid supervised options: check variance floor and "
                   "kNN neighbor count");
  if (stopped(ctl))
    return supervisedFailure("Cancelled", true);
  const auto d = x.dimensions;
  const Labeled set = labeled(x, labels);
  if (set.rows.empty())
    return supervisedFailure("All samples are unlabeled");
  const std::size_t c = set.classIds.size();
  if (c < 2)
    return supervisedFailure("Fewer than two labeled classes");
  const std::size_t n = set.rows.size();
  std::vector<std::size_t> counts(c, 0);
  for (int t : set.rowClass)
    counts[std::size_t(t)] += 1;
  if (o.method == SupervisedMethod::Lda && n - c < d)
    return supervisedFailure("LDA requires labeled samples minus classes to be at "
                   "least the dimension count (pooled covariance degrees "
                   "of freedom)");
  if (o.method == SupervisedMethod::Qda &&
      *std::min_element(counts.begin(), counts.end()) < d + 1)
    return supervisedFailure("QDA requires at least dimensions + 1 samples per class");
  SupervisedModel model;
  model.method = o.method;
  model.dimensions = d;
  model.classIds = set.classIds;
  model.priors.assign(c, 0);
  for (std::size_t k = 0; k < c; ++k)
    model.priors[k] = double(counts[k]) / double(n);
  Matrix samples{d, {}};
  samples.values.reserve(n * d);
  for (std::size_t idx = 0; idx < n; ++idx)
    samples.values.insert(samples.values.end(), x.row(set.rows[idx]),
                          x.row(set.rows[idx]) + d);
  if (o.method == SupervisedMethod::Knn) {
    model.trainValues = samples;
    model.trainLabels = set.rowClass;
    model.knnNeighbors =
        int(std::min<std::size_t>(std::size_t(o.knnNeighbors), n));
    SupervisedResult r = classified(samples, model, ctl, 0);
    if (r.ok)
      report(ctl, 1);
    return r;
  }
  const std::size_t blocks = o.method == SupervisedMethod::Lda ? 1 : c;
  std::vector<double> means(c * d, 0);
  for (std::size_t idx = 0; idx < n; ++idx) {
    if ((idx & 1023U) == 0) {
      if (stopped(ctl))
        return supervisedFailure("Cancelled", true);
      report(ctl, double(idx) / double(6 * n));
    }
    const std::size_t k = std::size_t(set.rowClass[idx]);
    for (std::size_t j = 0; j < d; ++j)
      means[k * d + j] += samples.row(idx)[j];
  }
  for (std::size_t k = 0; k < c; ++k)
    for (std::size_t j = 0; j < d; ++j)
      means[k * d + j] /= double(counts[k]);
  // Pooled (LDA) or per-class (QDA) centered outer products.
  std::vector<double> scatter(blocks * d * d, 0), deviation(n * d, 0);
  for (std::size_t idx = 0; idx < n; ++idx) {
    if ((idx & 1023U) == 0)
      report(ctl, (1.0 + double(idx) / double(n)) / 6.0);
    const std::size_t own = std::size_t(set.rowClass[idx]);
    for (std::size_t j = 0; j < d; ++j)
      deviation[idx * d + j] = samples.row(idx)[j] - means[own * d + j];
  }
  for (std::size_t idx = 0; idx < n; ++idx) {
    if ((idx & 1023U) == 0) {
      if (stopped(ctl))
        return supervisedFailure("Cancelled", true);
      report(ctl, (2.0 + double(idx) / double(n)) / 6.0);
    }
    const std::size_t own = std::size_t(set.rowClass[idx]);
    const std::size_t block = o.method == SupervisedMethod::Lda ? 0 : own;
    for (std::size_t a = 0; a < d; ++a)
      for (std::size_t b = 0; b < d; ++b)
        scatter[block * d * d + a * d + b] +=
            deviation[idx * d + a] * deviation[idx * d + b];
  }
  const double denom = double(n - c);
  for (std::size_t k = 0; k < blocks; ++k) {
    if (o.method == SupervisedMethod::Qda)
      for (std::size_t t = 0; t < d * d; ++t)
        scatter[k * d * d + t] /= double(counts[k] - 1);
    else
      for (std::size_t t = 0; t < d * d; ++t)
        scatter[k * d * d + t] /= denom;
    std::vector<double> block(scatter.begin() +
                                  std::ptrdiff_t(k) * std::ptrdiff_t(d * d),
                              scatter.begin() +
                                  std::ptrdiff_t(k + 1) *
                                      std::ptrdiff_t(d * d));
    std::vector<double> covariance;
    double logDet = 0;
    if (!factorScatter(block, d, o.varianceFloor, covariance, logDet))
      return supervisedFailure("Covariance matrix is not positive definite after "
                     "variance floor retries");
    model.covariances.insert(model.covariances.end(), covariance.begin(),
                             covariance.end());
  }
  model.means.dimensions = d;
  model.means.values = means;
  SupervisedResult r = classified(samples, model, ctl, 0.5);
  if (r.ok)
    report(ctl, 1);
  return r;
}

SupervisedResult predictSupervised(const Matrix &x,
                                   const SupervisedModel &m,
                                   const Control &ctl) {
  if (!validMatrix(x))
    return supervisedFailure("Non-finite sample values or zero dimensions");
  if (!m.dimensions || m.dimensions != x.dimensions)
    return supervisedFailure("Model dimensions do not match sample matrix");
  if (!consistentModel(m, m.dimensions))
    return supervisedFailure("Model parameters are inconsistent with its class list");
  if (stopped(ctl))
    return supervisedFailure("Cancelled", true);
  SupervisedResult r = classified(x, m, ctl, 0);
  if (r.ok)
    report(ctl, 1);
  return r;
}

CrossValidationResult crossValidate(const Matrix &x,
                                    const std::vector<int> &labels,
                                    const SupervisedOptions &o, int folds,
                                    std::uint64_t seed, const Control &ctl) {
  if (!validMatrix(x))
    return cvFailure("Non-finite sample values or zero dimensions");
  if (labels.size() != x.rows())
    return cvFailure("Label count does not match sample matrix rows");
  if (!validOptions(o))
    return cvFailure("Invalid supervised options: check variance floor and "
                     "kNN neighbor count");
  if (folds < 2)
    return cvFailure("Cross-validation folds must be at least two");
  const Labeled set = labeled(x, labels);
  if (set.rows.empty())
    return cvFailure("All samples are unlabeled");
  const std::size_t c = set.classIds.size();
  if (c < 2)
    return cvFailure("Fewer than two labeled classes");
  std::vector<std::size_t> counts(c, 0);
  for (int t : set.rowClass)
    counts[std::size_t(t)] += 1;
  const auto smallest = *std::min_element(counts.begin(), counts.end());
  if (smallest < 2)
    return cvFailure("Smallest class has fewer than two samples");
  const int f = std::min(folds, int(smallest));
  // Stratified assignment: per-class shuffled round-robin dealing.
  const std::size_t foldCount = std::size_t(f);
  std::vector<std::vector<std::size_t>> foldRows(foldCount);
  std::mt19937_64 rng(seed);
  for (std::size_t k = 0; k < c; ++k) {
    std::vector<std::size_t> members;
    for (std::size_t idx = 0; idx < set.rows.size(); ++idx)
      if (set.rowClass[idx] == int(k))
        members.push_back(idx);
    std::shuffle(members.begin(), members.end(), rng);
    for (std::size_t j = 0; j < members.size(); ++j)
      foldRows[j % std::size_t(f)].push_back(members[j]);
  }
  const auto d = x.dimensions;
  std::vector<std::vector<std::int64_t>> cells(
      c, std::vector<std::int64_t>(c, 0));
  for (int fold = 0; fold < f; ++fold) {
    if (stopped(ctl))
      return cvFailure("Cancelled", true);
    Control inner{ctl.cancelled, {}};
    const std::size_t foldIndex = std::size_t(fold);
    std::vector<bool> held(set.rows.size(), false);
    for (std::size_t idx : foldRows[foldIndex])
      held[idx] = true;
    Matrix train{d, {}}, test{d, {}};
    std::vector<int> trainLabels;
    std::vector<std::size_t> testIdx; // held labeled rows, ascending
    for (std::size_t idx = 0; idx < set.rows.size(); ++idx) {
      const double *row = x.row(set.rows[idx]);
      if (held[idx]) {
        test.values.insert(test.values.end(), row, row + d);
        testIdx.push_back(idx);
      } else {
        train.values.insert(train.values.end(), row, row + d);
        trainLabels.push_back(set.classIds[std::size_t(set.rowClass[idx])]);
      }
    }
    auto trained = trainSupervised(train, trainLabels, o, inner);
    if (!trained.ok) {
      if (trained.cancelled)
        return cvFailure("Fold training cancelled", true);
      return cvFailure("Fold " + std::to_string(fold) +
                       " training failed: " + trained.error);
    }
    auto predicted = predictSupervised(test, trained.model, inner);
    if (!predicted.ok) {
      if (predicted.cancelled)
        return cvFailure("Fold prediction cancelled", true);
      return cvFailure("Fold " + std::to_string(fold) +
                       " prediction failed: " + predicted.error);
    }
    // predicted.labels is parallel to the ascending test rows, not to the
    // class-blocked fold membership order.
    for (std::size_t j = 0; j < testIdx.size(); ++j)
      cells[std::size_t(set.rowClass[testIdx[j]])]
           [std::size_t(predicted.labels[j])] += 1;
    report(ctl, double(fold) / double(f));
  }
  CrossValidationResult out;
  out.folds = f;
  out.confusion.classIds = set.classIds;
  out.confusion.cells = cells;
  out.confusion.precision.assign(c, std::numeric_limits<double>::quiet_NaN());
  out.confusion.recall.assign(c, std::numeric_limits<double>::quiet_NaN());
  for (std::size_t k = 0; k < c; ++k) {
    std::int64_t rowSum = 0, colSum = 0;
    for (std::size_t t = 0; t < c; ++t) {
      rowSum += cells[k][t];
      colSum += cells[t][k];
    }
    if (rowSum)
      out.confusion.recall[k] = double(cells[k][k]) / double(rowSum);
    if (colSum)
      out.confusion.precision[k] = double(cells[k][k]) / double(colSum);
  }
  if (stopped(ctl))
    return cvFailure("Cancelled", true);
  out.ok = true;
  report(ctl, 1);
  return out;
}
} // namespace paleo::cluster
