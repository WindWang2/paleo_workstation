// 层：数据
#pragma once
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>
#include <cstddef>
#include <vector>

namespace paleo::crossplot {
struct Grid {
  int rows = 0, cols = 0;
  std::array<double, 6> transform{0, 1, 0, 0, 0, -1};
  QString crs;
  bool spatial = false;
};
struct Location {
  QString wellId;
  double depth = 0, x = 0, y = 0;
  bool hasXY = false;
  int pixel = -1;
};
struct SampleSet {
  QStringList names, units, parentVersionIds, sourceLayerIds;
  std::vector<double> values; // finite, row-major, names.size() dimensions
  QVector<Location> locations;
  Grid grid;
  qint64 rejected = 0;
  std::size_t rows() const {
    return names.isEmpty() ? 0 : values.size() / std::size_t(names.size());
  }
};
struct Axes {
  int x = 0, y = 1, z = -1;
  double yaw = 30, pitch = 20;
};
struct PlotPoint {
  float x = 0, y = 0;
  int sample = -1;
  int label = -1;
};
struct PlotFrame {
  QVector<PlotPoint> points; // normalized [0,1], also used for exact picking
  QVector<int> density;      // row-major 256² bins when >=100k
  int densitySide = 256, densityMax = 0;
  QString xTitle, yTitle, zTitle;
  double xMin = 0, xMax = 1, yMin = 0, yMax = 1;
  bool is3d = false;
};
struct Selection {
  QVector<int> indices;
  QVector<double> means;
  double fraction = 0;
  QStringList wellIds;
};
struct SourceChoice {
  QString id, title, kind;
};
} // namespace paleo::crossplot
Q_DECLARE_METATYPE(paleo::crossplot::Axes)
Q_DECLARE_METATYPE(paleo::crossplot::Selection)
