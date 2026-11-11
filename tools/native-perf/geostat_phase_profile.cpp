// Development-only component timing: include the authoritative kernel so its
// private components can be measured without adding hooks to production APIs.
#include "algorithms/geostat/kriging.cpp"
#include "algorithms/geostat/crossvalidation.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace paleo::geostat;
namespace {
using Clock = std::chrono::steady_clock;
qint64 elapsed(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}
struct Phases {
  qint64 dedupe = 0, index = 0, query = 0, solve = 0, release = 0;
  qint64 sortProbeCollect = 0, sortProbeSort = 0;
  qint64 indexSortProbeCollect = 0, indexSortProbeSort = 0;
  qint64 stableSortProbeCollect = 0, stableSortProbeSort = 0;
  std::uint64_t sortProbeChecksum = 0;
  std::uint64_t sortProbeComparisons = 0;
  std::uint64_t indexSortProbeComparisons = 0;
  std::uint64_t stableSortProbeComparisons = 0;
  int calls = 0;
};
std::vector<Sample> wells(int columns, int rows) {
  std::vector<Sample> result;
  for (int row = 0; row < rows; ++row) for (int col = 0; col < columns; ++col) {
    const double x = col * 20. + 6. * std::sin(12.9898 * col + 78.233 * row);
    const double y = row * 20. + 6. * std::cos(39.346 * row + 11.135 * col);
    result.push_back({x, y, std::sin(x / 70.) + std::cos(y / 90.) + 2.});
  }
  return result;
}
// This is a separate container-sort probe, not an exact subphase subtraction:
// its cache and allocation state differ from the full dedupe call above it.
void sortProbe(const std::vector<Sample> &rest, Phases &phases) {
  auto start = Clock::now();
  std::vector<std::pair<Sample, std::uint32_t>> sorted;
  sorted.reserve(rest.size());
  for (std::size_t i = 0; i < rest.size(); ++i)
    if (std::isfinite(rest[i].x) && std::isfinite(rest[i].y) && std::isfinite(rest[i].value))
      sorted.push_back({rest[i], std::uint32_t(i)});
  phases.sortProbeCollect += elapsed(start);
  start = Clock::now();
  std::sort(sorted.begin(), sorted.end(), [&phases](const auto &a, const auto &b) {
    ++phases.sortProbeComparisons;
    if (a.first.x != b.first.x) return a.first.x < b.first.x;
    if (a.first.y != b.first.y) return a.first.y < b.first.y;
    return a.second < b.second;
  });
  phases.sortProbeSort += elapsed(start);
  // Observe the entire permutation outside the timed sort.
  for (const auto &entry : sorted)
    phases.sortProbeChecksum = phases.sortProbeChecksum * 1099511628211ULL + entry.second;

  start = Clock::now();
  std::vector<std::uint32_t> indices;
  indices.reserve(rest.size());
  for (std::size_t i = 0; i < rest.size(); ++i)
    if (std::isfinite(rest[i].x) && std::isfinite(rest[i].y) && std::isfinite(rest[i].value))
      indices.push_back(std::uint32_t(i));
  phases.indexSortProbeCollect += elapsed(start);
  start = Clock::now();
  std::sort(indices.begin(), indices.end(), [&rest, &phases](auto a, auto b) {
    ++phases.indexSortProbeComparisons;
    if (rest[a].x != rest[b].x) return rest[a].x < rest[b].x;
    if (rest[a].y != rest[b].y) return rest[a].y < rest[b].y;
    return a < b;
  });
  phases.indexSortProbeSort += elapsed(start);
  if (indices.size() != sorted.size()) throw std::runtime_error("sort probes disagree on finite input");
  for (std::size_t i = 0; i < indices.size(); ++i)
    if (indices[i] != sorted[i].second) throw std::runtime_error("index sort changed point order");

  start = Clock::now();
  std::vector<std::uint32_t> stableIndices;
  stableIndices.reserve(rest.size());
  for (std::size_t i = 0; i < rest.size(); ++i)
    if (std::isfinite(rest[i].x) && std::isfinite(rest[i].y) && std::isfinite(rest[i].value))
      stableIndices.push_back(std::uint32_t(i));
  phases.stableSortProbeCollect += elapsed(start);
  start = Clock::now();
  std::stable_sort(stableIndices.begin(), stableIndices.end(), [&rest, &phases](auto a, auto b) {
    ++phases.stableSortProbeComparisons;
    if (rest[a].x != rest[b].x) return rest[a].x < rest[b].x;
    if (rest[a].y != rest[b].y) return rest[a].y < rest[b].y;
    return a < b;
  });
  phases.stableSortProbeSort += elapsed(start);
  if (stableIndices != indices) throw std::runtime_error("stable index sort changed point order");
}
QJsonObject run(int columns, int rows) {
  const auto samples = wells(columns, rows);
  VariogramModel model; model.type = VariogramModelType::Spherical;
  model.nugget = .05; model.sill = 1; model.range = 90;
  KrigingParams params; params.maxPoints = 16;
  Phases phases;
  const auto predict = [&](double x, double y, const std::vector<Sample> &rest) {
    CvPrediction prediction;
    auto releaseStart = Clock::now();
    {
      SolverCore core({}, model, params);
      {
        int merged = 0;
        auto start = Clock::now();
        auto deduped = detail::dedupeSamples(rest, &merged);
        phases.dedupe += elapsed(start);
        start = Clock::now();
        core.index = detail::NeighborIndex::build(deduped);
        phases.index += elapsed(start);
        core.merged = merged; core.valid = !deduped.empty();
        releaseStart = Clock::now();
      }
      phases.release += elapsed(releaseStart);
      std::vector<std::uint32_t> neighborhood;
      auto start = Clock::now();
      const bool found = core.neighborhood(x, y, &neighborhood);
      phases.query += elapsed(start);
      KrigingPointResult point;
      start = Clock::now();
      const bool solved = found && solveOrdinaryKriging(neighborhood, core.index, x, y, core.model, &point);
      phases.solve += elapsed(start);
      prediction = {solved, point.estimate, point.variance};
      releaseStart = Clock::now();
    }
    phases.release += elapsed(releaseStart);
    ++phases.calls;
    sortProbe(rest, phases);
    return prediction;
  };
  bool cancelled = false;
  const auto start = Clock::now();
  const auto result = leaveOneOut(samples, "ordinary_kriging", predict, {}, &cancelled);
  const auto total = elapsed(start);
  const auto reference = leaveOneOut(samples, "ordinary_kriging", [&](double x, double y, const auto &rest) {
    const auto point = ordinaryKrigingAt(x, y, rest, model, params);
    return CvPrediction{point.ok, point.estimate, point.variance};
  }, {}, &cancelled);
  if (cancelled || result != reference) throw std::runtime_error("profile changed CV numeric results");
  return {{"samples", int(samples.size())}, {"calls", phases.calls},
      {"profile_total_ms_including_sort_probe", total / 1e6}, {"dedupe_ms", phases.dedupe / 1e6},
      {"index_build_ms", phases.index / 1e6}, {"query_ms", phases.query / 1e6},
      {"solve_ms", phases.solve / 1e6}, {"release_ms", phases.release / 1e6},
      {"independent_sort_probe_collect_ms", phases.sortProbeCollect / 1e6},
      {"independent_sort_probe_sort_ms", phases.sortProbeSort / 1e6},
      {"sort_probe_checksum", QString::number(phases.sortProbeChecksum)},
      {"sort_probe_comparisons", double(phases.sortProbeComparisons)},
      {"independent_index_sort_probe_collect_ms", phases.indexSortProbeCollect / 1e6},
      {"independent_index_sort_probe_sort_ms", phases.indexSortProbeSort / 1e6},
      {"index_sort_probe_comparisons", double(phases.indexSortProbeComparisons)},
      {"index_sort_preserves_pair_order", true},
      {"independent_stable_index_sort_probe_collect_ms", phases.stableSortProbeCollect / 1e6},
      {"independent_stable_index_sort_probe_sort_ms", phases.stableSortProbeSort / 1e6},
      {"stable_index_sort_probe_comparisons", double(phases.stableSortProbeComparisons)},
      {"stable_index_sort_preserves_pair_order", true},
      {"matches_public_cv", true}, {"rmse", result.value("rmse").toDouble()}};
}
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    const QJsonObject result{{"at_cap", run(50, 40)}, {"above_cap", run(100, 80)}};
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Indented).constData();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
