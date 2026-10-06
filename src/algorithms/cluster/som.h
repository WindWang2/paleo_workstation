// 层：数据
#pragma once
#include "cluster.h"
#include <cstdint>
#include <string>
#include <vector>

namespace paleo::cluster {
// Online self-organizing map (Kohonen). Prototypes sit on a width x height
// grid; the prototype index is y * width + x. Every epoch presents the samples
// in a deterministic shuffle seeded from `seed`, pulls the best-matching unit
// (BMU, Euclidean distance) towards the sample and drags the neighbourhood
// along. Both control schedules decay exponentially across epochs: v(e) =
// v0 * (v1 / v0) ^ (e / epochs) for epoch e in [0, epochs), so epoch 0 uses the
// initial value and epoch `epochs` would reach the final one. The influence of
// a unit u on the BMU update is Gaussian in grid distance: h(u) =
// exp(-gridDist(u, bmu)^2 / (2 * radius(e)^2)). No geological meaning is
// inferred from label numbers; adapters map them onto real classes.
struct SomOptions {
  int width = 4, height = 4; // prototype grid; classes = width * height
  int epochs = 50;
  double initialLearningRate = 0.5, finalLearningRate = 0.05;
  double initialRadius = 0; // 0 -> max(width, height) / 2.0
  double finalRadius = 1.0;
  std::uint64_t seed = 42;
};
struct SomResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;
  Matrix prototypes; // (width * height) x dimensions, index = y * width + x
  int width = 0, height = 0;
  std::vector<int> labels; // per-row BMU prototype index (0-based)
  // clamp(1 - best/second, 0, 1): 1 with a single prototype, 0 when the two
  // best prototypes coincide (second == 0) and the margin carries no signal.
  std::vector<double> confidence;
  std::vector<double> squaredDistance; // squared distance to the BMU
  // per-epoch mean BMU squared distance (quantization error)
  std::vector<double> quantizationErrorHistory;
};
SomResult som(const Matrix &, const SomOptions & = {}, const Control & = {});
} // namespace paleo::cluster
