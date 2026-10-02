// 层：数据
#pragma once
#include "algorithms/cluster/cluster.h"
#include "domain/crossplotsamples.h"
#include "io/lasdoc.h"
#include <QPointF>

namespace paleo::crossplot {
struct Channel {
  QString name, unit, wellId, versionId;
  QVector<double> depths, values;
  double x = 0, y = 0;
  bool hasXY = false;
};
struct RasterSource {
  QString path, name, versionId, layerId;
  int band = 1;
};
struct Plane {
  QString name, versionId;
  Grid grid;
  std::vector<double> values;
};
struct AttributeSection {
  Plane plane;
  QVector<QPointF> traceXY;
  double startTimeMs = 0, stepMs = 0;
};
struct SampleResult {
  bool ok = false, cancelled = false;
  QString error;
  SampleSet samples;
};
class CrossplotSamples {
public:
  static SampleResult well(const QVector<Channel> &,
                           const cluster::Control & = {});
  static SampleResult las(const QString &path, const QStringList &curves,
                          const Location &well, const QString &versionId,
                          const cluster::Control & = {});
  static SampleResult planes(const QVector<Plane> &,
                             const cluster::Control & = {});
  static SampleResult rasters(const QVector<RasterSource> &,
                              const cluster::Control & = {});
  // TWT horizon (ms), one attribute value per trace at the picked time.
  static SampleResult attributeHorizon(const QVector<AttributeSection> &,
                                       const QVector<Plane> &,
                                       const cluster::Control & = {});
  static PlotFrame project(const SampleSet &, const Axes &,
                           const std::vector<int> &labels = {});
  static Selection select(const SampleSet &, const PlotFrame &,
                          const QVector<QPointF> &lasso);
  static int nearest(const PlotFrame &, QPointF point, double radius);
  static bool validate(const SampleSet &, QString *error = nullptr);
};
} // namespace paleo::crossplot
