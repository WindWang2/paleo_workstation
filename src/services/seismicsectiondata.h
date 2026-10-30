// 层：数据
#pragma once
#include <QString>
#include <vector>

namespace seismic {
// Numerical section: rows increase in absolute TWT, independent of texture orientation.
struct SeismicSectionData {
  int columns=0, samples=0;
  double firstTimeMs=0, sampleIntervalMs=0;
  QString timeReference;
  std::vector<double> distancesM;
  std::vector<float> values; // [sample * columns + column], original amplitudes
};
}
