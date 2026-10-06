// 层：数据
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace paleo::cluster {
// Row-major finite samples. Sampling adapters remove missing rows, never
// impute.
struct Matrix {
  std::size_t dimensions = 0;
  std::vector<double> values;
  std::size_t rows() const {
    return dimensions ? values.size() / dimensions : 0;
  }
  const double *row(std::size_t i) const {
    return values.data() + i * dimensions;
  }
};
struct Control {
  std::function<bool()> cancelled;
  std::function<void(double)>
      progress; // monotonic [0,1]; completion only on success
};
struct Options {
  int k = 2;
  int maxIterations = 100;
  std::uint64_t seed = 42;
  double tolerance = 1e-6;
  double varianceFloor = 1e-6;
  std::size_t emChunkBudgetBytes = 268435456; // EM memory budget; 0 = full path
};
enum class Method { KMeans, Gmm };
struct Model {
  Method method = Method::KMeans;
  Matrix centers;
  std::vector<double> variances; // GMM diagonal, same layout as centers
  std::vector<double> weights;
};
struct Result {
  bool ok = false;
  bool cancelled = false;
  std::string error;
  Model model;
  std::vector<int> labels; // 0-based; no geological meaning is inferred
  std::vector<double>
      confidence; // kmeans relative distance margin; GMM posterior
  std::vector<double> squaredDistance;
  std::vector<double>
      objectiveHistory; // SSE for kmeans, observed log-likelihood for GMM
  double bic = 0;
  int iterations = 0;
  bool chunkedEm = false; // gmm() ran chunked EM (no n*k buffer)
};
Result kmeans(const Matrix &, const Options & = {}, const Control & = {});
Result gmm(const Matrix &, const Options & = {}, const Control & = {});
Result predict(const Matrix &, const Model &, const Control & = {});

struct Point {
  double x = 0, y = 0;
};
std::vector<Point> convexHull(std::vector<Point> points);
// Inclusive boundary, even-odd interior. The original lasso order is preserved.
bool inPolygon(Point point, const std::vector<Point> &vertices);
bool inBox(const double *point, const std::vector<double> &lo,
           const std::vector<double> &hi);
} // namespace paleo::cluster
