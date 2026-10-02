// 层：数据
#include "crossplotsamples.h"
#include "io/lascache.h"
#include <algorithm>
#include <cmath>
#include <gdal.h>
#include <limits>
#include <memory>
#include <ogr_srs_api.h>

namespace paleo::crossplot {
namespace {
bool stop(const cluster::Control &c) { return c.cancelled && c.cancelled(); }
void progress(const cluster::Control &c, double p) {
  if (c.progress)
    c.progress(p);
}
SampleResult fail(const QString &e, bool cancel = false) {
  SampleResult r;
  r.error = e;
  r.cancelled = cancel;
  return r;
}
void parent(SampleSet &s, const QString &id) {
  if (!id.isEmpty() && !s.parentVersionIds.contains(id))
    s.parentVersionIds << id;
}
bool goodGrid(const Grid &g) {
  return g.rows > 0 && g.cols > 0 &&
         qint64(g.rows) * g.cols <= std::numeric_limits<int>::max() &&
         std::all_of(g.transform.begin(), g.transform.end(),
                     [](double v) { return std::isfinite(v); }) &&
         g.transform[1] * g.transform[5] - g.transform[2] * g.transform[4] != 0;
}
void xy(const Grid &g, int pixel, double &x, double &y) {
  const double col = pixel % g.cols + .5, row = pixel / g.cols + .5;
  x = g.transform[0] + col * g.transform[1] + row * g.transform[2];
  y = g.transform[3] + col * g.transform[4] + row * g.transform[5];
}
} // namespace
bool CrossplotSamples::validate(const SampleSet &s, QString *error) {
  const bool ok =
      !s.names.isEmpty() && s.units.size() == s.names.size() &&
      s.values.size() % std::size_t(s.names.size()) == 0 &&
      s.rows() == std::size_t(s.locations.size()) &&
      s.rows() <= std::size_t(std::numeric_limits<int>::max()) &&
      (!s.grid.spatial || goodGrid(s.grid)) && s.rejected >= 0 &&
      std::all_of(s.values.begin(), s.values.end(),
                  [](double v) { return std::isfinite(v); }) &&
      std::all_of(s.locations.begin(), s.locations.end(),
                  [](const Location &loc) {
                    return (!loc.hasXY ||
                            (std::isfinite(loc.x) && std::isfinite(loc.y))) &&
                           (loc.wellId.isEmpty() || std::isfinite(loc.depth));
                  });
  if (!ok && error)
    *error = QStringLiteral("样本维度、位置或有限值契约不成立");
  return ok;
}
SampleResult CrossplotSamples::well(const QVector<Channel> &channels,
                                    const cluster::Control &ctl) {
  if (channels.size() < 2)
    return fail(QStringLiteral("交会至少需要两个通道"));
  SampleResult r;
  auto &s = r.samples;
  s.samplingMetadata = {{"method", "reference_depth_linear_no_extrapolation"},
                        {"nanPolicy", "joint_valid_rows_no_nan_bridging"}};
  for (const Channel &c : channels) {
    if (c.depths.size() != c.values.size() || c.wellId != channels[0].wellId)
      return fail(QStringLiteral("曲线长度或井标识不一致"));
    for (int i = 0; i < c.depths.size(); ++i)
      if (!std::isfinite(c.depths[i]) || (i && c.depths[i] <= c.depths[i - 1]))
        return fail(QStringLiteral("源深度必须有限且严格递增"));
    s.names << c.name;
    s.units << c.unit;
    parent(s, c.versionId);
  }
  QVector<int> cursor(channels.size(), 0);
  std::vector<double> row(std::size_t(channels.size()));
  for (int i = 0; i < channels[0].depths.size(); ++i) {
    if ((i & 1023) == 0) {
      if (stop(ctl))
        return fail(QStringLiteral("已取消"), true);
      progress(ctl,
               double(i) / std::max(qsizetype(1), channels[0].depths.size()));
    }
    const double depth = channels[0].depths[i];
    bool good = true;
    for (int j = 0; j < channels.size(); ++j) {
      const auto &c = channels[j];
      int &at = cursor[j];
      while (at < c.depths.size() && c.depths[at] < depth)
        ++at;
      double v = std::numeric_limits<double>::quiet_NaN();
      if (at < c.depths.size() && c.depths[at] == depth)
        v = c.values[at];
      else if (at > 0 && at < c.depths.size() &&
               std::isfinite(c.values[at - 1]) && std::isfinite(c.values[at]))
        v = c.values[at - 1] + (c.values[at] - c.values[at - 1]) *
                                   (depth - c.depths[at - 1]) /
                                   (c.depths[at] - c.depths[at - 1]);
      row[std::size_t(j)] = v;
      good = good && std::isfinite(v);
    }
    if (!good) {
      ++s.rejected;
      continue;
    }
    s.values.insert(s.values.end(), row.begin(), row.end());
    s.locations.append({channels[0].wellId, depth, channels[0].x, channels[0].y,
                        channels[0].hasXY, -1, i});
  }
  if (stop(ctl))
    return fail(QStringLiteral("已取消"), true);
  r.ok = true;
  progress(ctl, 1);
  return r;
}
SampleResult CrossplotSamples::las(const QString &path,
                                   const QStringList &names,
                                   const Location &loc, const QString &version,
                                   const cluster::Control &ctl) {
  if (stop(ctl))
    return fail(QStringLiteral("已取消"), true);
  const LasDoc doc = LasCache::shared().load(path);
  if (!doc.ok)
    return fail(doc.error);
  if (doc.curves.isEmpty())
    return fail(QStringLiteral("LAS 无深度曲线"));
  QVector<Channel> channels;
  for (const QString &name : names) {
    auto it = std::find_if(doc.curves.begin(), doc.curves.end(),
                           [&](const LasCurve &c) { return c.name == name; });
    if (it == doc.curves.end())
      return fail(QStringLiteral("LAS 缺曲线 %1").arg(name));
    channels.append({it->name, it->unit, loc.wellId, version,
                     doc.curves[0].values, it->values, loc.x, loc.y,
                     loc.hasXY});
  }
  return well(channels, ctl);
}
SampleResult CrossplotSamples::planes(const QVector<Plane> &planes,
                                      const cluster::Control &ctl) {
  if (planes.size() < 2 || !goodGrid(planes[0].grid))
    return fail(QStringLiteral("至少两个通道且参考网格必须有效"));
  SampleResult r;
  auto &s = r.samples;
  s.grid = planes[0].grid;
  const auto size = std::size_t(s.grid.rows) * std::size_t(s.grid.cols);
  for (const auto &p : planes) {
    if (p.grid.rows != s.grid.rows || p.grid.cols != s.grid.cols ||
        p.values.size() != size || p.grid.transform != s.grid.transform ||
        p.grid.crs != s.grid.crs || p.grid.spatial != s.grid.spatial)
      return fail(
          QStringLiteral("属性平面与参考网格不一致；请显式采样到同一网格"));
    s.names << p.name;
    s.units << QString();
    parent(s, p.versionId);
  }
  s.values.reserve(size * std::size_t(planes.size()));
  s.locations.reserve(qsizetype(size));
  for (std::size_t i = 0; i < size; ++i) {
    if ((i & 1023U) == 0) {
      if (stop(ctl))
        return fail(QStringLiteral("已取消"), true);
      progress(ctl, double(i) / double(size));
    }
    bool good = true;
    for (const auto &p : planes)
      good = good && std::isfinite(p.values[i]);
    if (!good) {
      ++s.rejected;
      continue;
    }
    for (const auto &p : planes)
      s.values.push_back(p.values[i]);
    Location loc;
    loc.pixel = int(i);
    loc.hasXY = s.grid.spatial;
    xy(s.grid, int(i), loc.x, loc.y);
    s.locations << loc;
  }
  if (stop(ctl))
    return fail(QStringLiteral("已取消"), true);
  r.ok = true;
  progress(ctl, 1);
  return r;
}
SampleResult CrossplotSamples::rasters(const QVector<RasterSource> &sources,
                                       const cluster::Control &ctl) {
  if (sources.size() < 2)
    return fail(QStringLiteral("交会至少需要两张栅格"));
  GDALAllRegister();
  QVector<Plane> planes;
  Grid reference;
  for (int si = 0; si < sources.size(); ++si) {
    if (stop(ctl))
      return fail(QStringLiteral("已取消"), true);
    const auto &src = sources[si];
    std::unique_ptr<void, decltype(&GDALClose)> ds(
        GDALOpen(src.path.toUtf8().constData(), GA_ReadOnly), GDALClose);
    if (!ds)
      return fail(QStringLiteral("无法读栅格 %1（SATR 垂直切片需显式空间采样）")
                      .arg(src.path));
    Grid grid;
    grid.cols = GDALGetRasterXSize(ds.get());
    grid.rows = GDALGetRasterYSize(ds.get());
    grid.crs = QString::fromUtf8(GDALGetProjectionRef(ds.get()));
    grid.spatial = true;
    if (GDALGetGeoTransform(ds.get(), grid.transform.data()) != CE_None ||
        !goodGrid(grid))
      return fail(QStringLiteral("栅格缺有效仿射网格"));
    if (si == 0)
      reference = grid;
    GDALRasterBandH band = GDALGetRasterBand(ds.get(), src.band);
    if (!band)
      return fail(QStringLiteral("栅格波段不存在"));
    const auto count = std::size_t(grid.cols) * std::size_t(grid.rows);
    std::vector<double> values(count);
    std::vector<unsigned char> mask(count);
    // Row strips keep cancellation responsive while retaining sequential GDAL
    // reads.
    int hasNo = 0;
    const double nodata = GDALGetRasterNoDataValue(band, &hasNo);
    for (int row = 0; row < grid.rows; row += 64) {
      if (stop(ctl))
        return fail(QStringLiteral("已取消"), true);
      const int h = std::min(64, grid.rows - row);
      if (GDALRasterIO(band, GF_Read, 0, row, grid.cols, h,
                       values.data() + std::size_t(row) * grid.cols, grid.cols,
                       h, GDT_Float64, 0, 0) != CE_None ||
          GDALRasterIO(GDALGetMaskBand(band), GF_Read, 0, row, grid.cols, h,
                       mask.data() + std::size_t(row) * grid.cols, grid.cols, h,
                       GDT_Byte, 0, 0) != CE_None)
        return fail(QStringLiteral("栅格像元读取失败"));
    }
    for (std::size_t i = 0; i < count; ++i)
      if (!mask[i] || (hasNo && values[i] == nodata))
        values[i] = std::numeric_limits<double>::quiet_NaN();
    Plane p;
    p.name = src.name;
    p.versionId = src.versionId;
    p.grid = reference;
    p.values.resize(std::size_t(reference.cols) * reference.rows,
                    std::numeric_limits<double>::quiet_NaN());
    if (reference.crs.isEmpty() != grid.crs.isEmpty())
      return fail(QStringLiteral("不能混采已知与未知 CRS"));
    OGRSpatialReferenceH a = nullptr, b = nullptr;
    OGRCoordinateTransformationH transform = nullptr;
    if (reference.crs != grid.crs) {
      a = OSRNewSpatialReference(reference.crs.toUtf8().constData());
      b = OSRNewSpatialReference(grid.crs.toUtf8().constData());
      if (a && b) {
        OSRSetAxisMappingStrategy(a, OAMS_TRADITIONAL_GIS_ORDER);
        OSRSetAxisMappingStrategy(b, OAMS_TRADITIONAL_GIS_ORDER);
        transform = OCTNewCoordinateTransformation(a, b);
      }
      if (!transform) {
        if (a)
          OSRDestroySpatialReference(a);
        if (b)
          OSRDestroySpatialReference(b);
        return fail(QStringLiteral("CRS 转换不可用"));
      }
    }
    double inverse[6];
    const bool invertible = GDALInvGeoTransform(grid.transform.data(), inverse);
    bool cancelled = false;
    if (invertible)
      for (std::size_t i = 0; i < p.values.size(); ++i) {
        if ((i & 1023U) == 0 && stop(ctl)) {
          cancelled = true;
          break;
        }
        double x, y;
        xy(reference, int(i), x, y);
        if (transform && !OCTTransform(transform, 1, &x, &y, nullptr))
          continue;
        const double cx = inverse[0] + inverse[1] * x + inverse[2] * y,
                     cy = inverse[3] + inverse[4] * x + inverse[5] * y;
        if (cx >= 0 && cy >= 0 && cx < grid.cols && cy < grid.rows)
          p.values[i] =
              values[std::size_t(int(cy)) * grid.cols + std::size_t(int(cx))];
      }
    if (transform)
      OCTDestroyCoordinateTransformation(transform);
    if (a)
      OSRDestroySpatialReference(a);
    if (b)
      OSRDestroySpatialReference(b);
    if (cancelled)
      return fail(QStringLiteral("已取消"), true);
    if (!invertible)
      return fail(QStringLiteral("仿射网格不可逆"));
    planes << std::move(p);
    progress(ctl, .8 * double(si + 1) / sources.size());
  }
  cluster::Control tail{ctl.cancelled,
                        [&](double p) { progress(ctl, .8 + .2 * p); }};
  auto r = CrossplotSamples::planes(planes, tail);
  r.samples.samplingMetadata = {
      {"method", "reference_pixel_center_containing_pixel"},
      {"nanPolicy", "joint_finite_nodata_mask"}};
  for (const auto &s : sources)
    if (!s.layerId.isEmpty())
      r.samples.sourceLayerIds << s.layerId;
  return r;
}
SampleResult
CrossplotSamples::attributeHorizon(const QVector<AttributeSection> &sections,
                                   const QVector<Plane> &horizons,
                                   const cluster::Control &ctl) {
  if (sections.isEmpty() || horizons.isEmpty() || !goodGrid(horizons[0].grid) ||
      !horizons[0].grid.spatial)
    return fail(QStringLiteral("需要属性剖面和时间层位平面（毫秒）"));
  const Grid grid = horizons[0].grid;
  for (const auto &h : horizons)
    if (h.grid.transform != grid.transform || h.grid.rows != grid.rows ||
        h.grid.cols != grid.cols || h.grid.crs != grid.crs ||
        h.values.size() != std::size_t(grid.rows) * grid.cols)
      return fail(QStringLiteral("层位平面网格不一致"));
  const auto &first = sections[0];
  for (const auto &a : sections)
    if (a.traceXY != first.traceXY ||
        a.plane.grid.rows != first.plane.grid.rows ||
        a.plane.grid.cols != first.plane.grid.cols ||
        a.plane.grid.cols != a.traceXY.size() ||
        a.plane.values.size() !=
            std::size_t(a.plane.grid.rows) * a.plane.grid.cols ||
        a.stepMs <= 0 || !std::isfinite(a.stepMs) ||
        !std::isfinite(a.startTimeMs) || a.plane.grid.crs != grid.crs)
      return fail(QStringLiteral("属性剖面几何或时间轴不一致"));
  SampleResult r;
  auto &s = r.samples;
  s.grid = grid;
  s.samplingMetadata = {
      {"method", "nearest_attribute_at_time_horizon_pixel_mean"},
      {"timeUnit", "ms"},
      {"startTimeMs", first.startTimeMs},
      {"stepMs", first.stepMs},
      {"geometryRmsResidual", first.geometryRmsResidual},
      {"geometryMaxResidual", first.geometryMaxResidual}};
  for (const auto &a : sections) {
    s.names << a.plane.name;
    s.units << QString();
    parent(s, a.plane.versionId);
  }
  for (const auto &h : horizons) {
    s.names << h.name;
    s.units << QStringLiteral("ms");
    parent(s, h.versionId);
  }
  QVariantList attributeAxes;
  for (const auto &a : sections)
    attributeAxes << QVariant(
        QVariantMap{{"versionId", a.plane.versionId},
                    {"startTimeMs", a.startTimeMs},
                    {"stepMs", a.stepMs},
                    {"geometryRmsResidual", a.geometryRmsResidual},
                    {"geometryMaxResidual", a.geometryMaxResidual}});
  s.samplingMetadata.insert("attributeAxes", attributeAxes);
  QVector<int> pixelRows(grid.rows * grid.cols, -1), counts;
  qint64 validTraces = 0;
  double inv[6];
  auto transform = grid.transform;
  if (!GDALInvGeoTransform(transform.data(), inv))
    return fail(QStringLiteral("网格不可逆"));
  for (int col = 0; col < first.traceXY.size(); ++col) {
    if (stop(ctl))
      return fail(QStringLiteral("已取消"), true);
    const auto point = first.traceXY[col];
    const double gx = inv[0] + inv[1] * point.x() + inv[2] * point.y(),
                 gy = inv[3] + inv[4] * point.x() + inv[5] * point.y();
    if (!std::isfinite(gx) || !std::isfinite(gy) || gx < 0 || gy < 0 ||
        gx >= grid.cols || gy >= grid.rows) {
      ++s.rejected;
      continue;
    }
    const int pixel = int(gy) * grid.cols + int(gx);
    const double time = horizons[0].values[std::size_t(pixel)];
    std::vector<double> row;
    bool good = std::isfinite(time);
    for (const auto &a : sections) {
      const double sample = (time - a.startTimeMs) / a.stepMs;
      double value = std::numeric_limits<double>::quiet_NaN();
      if (std::isfinite(sample) && sample >= 0 &&
          sample <= a.plane.grid.rows - 1) {
        const int at = int(std::round(sample));
        value = a.plane.values[std::size_t(a.plane.grid.rows - 1 - at) *
                                   a.plane.grid.cols +
                               std::size_t(col)];
      }
      good = good && std::isfinite(value);
      row.push_back(value);
    }
    for (const auto &h : horizons) {
      const double value = h.values[std::size_t(pixel)];
      good = good && std::isfinite(value);
      row.push_back(value);
    }
    if (!good) {
      ++s.rejected;
      continue;
    }
    ++validTraces;
    int &sampleRow = pixelRows[pixel];
    if (sampleRow < 0) {
      sampleRow = s.locations.size();
      counts << 1;
      s.values.insert(s.values.end(), row.begin(), row.end());
      Location loc;
      loc.pixel = pixel;
      loc.hasXY = true;
      xy(grid, pixel, loc.x, loc.y);
      s.locations << loc;
    } else {
      const double count = ++counts[sampleRow];
      for (std::size_t j = 0; j < row.size(); ++j) {
        auto &value = s.values[std::size_t(sampleRow) * row.size() + j];
        value += (row[j] - value) / count;
      }
    }
    progress(ctl, double(col + 1) / first.traceXY.size());
  }
  s.samplingMetadata.insert("validTraces", validTraces);
  s.samplingMetadata.insert("uniquePixels", qint64(s.rows()));
  s.samplingMetadata.insert("collapsedTraces", validTraces - qint64(s.rows()));
  if (stop(ctl))
    return fail(QStringLiteral("已取消"), true);
  r.ok = true;
  progress(ctl, 1);
  return r;
}
PlotFrame CrossplotSamples::project(const SampleSet &s, const Axes &a,
                                    const std::vector<int> &labels) {
  PlotFrame f;
  if (!validate(s) || a.x < 0 || a.y < 0 || a.x >= s.names.size() ||
      a.y >= s.names.size() || a.z < -1 || a.z >= s.names.size() ||
      !std::isfinite(a.yaw) || !std::isfinite(a.pitch) ||
      (labels.size() == s.rows() &&
       std::any_of(
           labels.begin(), labels.end(),
           [](int label) { return label < -1 || label > 254; })))
    return f;
  const auto d = std::size_t(s.names.size()), n = s.rows();
  f.xTitle = s.names[a.x];
  f.yTitle = s.names[a.y];
  f.is3d = a.z >= 0;
  if (f.is3d)
    f.zTitle = s.names[a.z];
  if (!n)
    return f;
  std::vector<double> lo(d, std::numeric_limits<double>::infinity()),
      hi(d, -std::numeric_limits<double>::infinity());
  for (std::size_t i = 0; i < n; ++i)
    for (int axis : {a.x, a.y, a.z})
      if (axis >= 0) {
        const auto j = std::size_t(axis);
        lo[j] = std::min(lo[j], s.values[i * d + j]);
        hi[j] = std::max(hi[j], s.values[i * d + j]);
      }
  f.xMin = lo[std::size_t(a.x)];
  f.xMax = hi[std::size_t(a.x)];
  f.yMin = lo[std::size_t(a.y)];
  f.yMax = hi[std::size_t(a.y)];
  const double yaw = a.yaw * 3.141592653589793 / 180,
               pitch = a.pitch * 3.141592653589793 / 180;
  auto norm = [&](std::size_t i, int axis) {
    const auto j = std::size_t(axis);
    return hi[j] > lo[j] ? (s.values[i * d + j] - lo[j]) / (hi[j] - lo[j]) : .5;
  };
  f.points.reserve(qsizetype(n));
  if (n >= 100000)
    f.density.fill(0, f.densitySide * f.densitySide);
  const int classCount =
      labels.size() == n ? 1 + *std::max_element(labels.begin(), labels.end())
                         : 0;
  std::vector<int> classBins;
  if (!f.density.isEmpty() && classCount > 0 && classCount <= 255) {
    classBins.assign(std::size_t(f.density.size()) * std::size_t(classCount),
                     0);
    f.densityClass.fill(-1, f.density.size());
  }
  for (std::size_t i = 0; i < n; ++i) {
    double x = norm(i, a.x), y = norm(i, a.y);
    if (f.is3d) {
      const double z = norm(i, a.z) - .5, u = x - .5, v = y - .5;
      const double rx = std::cos(yaw) * u - std::sin(yaw) * z;
      const double rz = std::sin(yaw) * u + std::cos(yaw) * z;
      x = .5 + rx / 1.8;
      y = .5 + (std::cos(pitch) * v - std::sin(pitch) * rz) / 1.8;
    }
    f.points << PlotPoint{x, y, int(i), labels.size() == n ? labels[i] : -1};
    if (!f.density.isEmpty()) {
      const int col = std::clamp(int(x * f.densitySide), 0, f.densitySide - 1),
                row = std::clamp(int(y * f.densitySide), 0, f.densitySide - 1);
      f.densityMax =
          std::max(f.densityMax, ++f.density[row * f.densitySide + col]);
      if (!classBins.empty() && labels[i] >= 0)
        ++classBins[std::size_t(row * f.densitySide + col) *
                        std::size_t(classCount) +
                    std::size_t(labels[i])];
    }
  }
  if (!classBins.empty())
    for (int bin = 0; bin < f.density.size(); ++bin) {
      const auto begin = classBins.begin() + std::ptrdiff_t(bin) * classCount;
      const auto winner = std::max_element(begin, begin + classCount);
      if (*winner > 0)
        f.densityClass[bin] = int(winner - begin);
    }
  return f;
}
Selection CrossplotSamples::select(const SampleSet &s, const PlotFrame &f,
                                   const QVector<QPointF> &polygon) {
  Selection selection;
  if (!validate(s) || polygon.size() < 3)
    return selection;
  std::vector<cluster::Point> vertices;
  for (auto p : polygon)
    vertices.push_back({p.x(), p.y()});
  selection.means.fill(0, s.names.size());
  for (const auto &p : f.points)
    if (p.sample >= 0 && std::size_t(p.sample) < s.rows() &&
        cluster::inPolygon({p.x, p.y}, vertices)) {
      selection.indices << p.sample;
      const auto &loc = s.locations[p.sample];
      if (!loc.wellId.isEmpty() && !selection.wellIds.contains(loc.wellId))
        selection.wellIds << loc.wellId;
      for (int j = 0; j < s.names.size(); ++j)
        selection.means[j] +=
            s.values[std::size_t(p.sample) * std::size_t(s.names.size()) +
                     std::size_t(j)];
    }
  if (!selection.indices.isEmpty())
    for (double &v : selection.means)
      v /= selection.indices.size();
  selection.fraction =
      s.rows() ? double(selection.indices.size()) / double(s.rows()) : 0;
  return selection;
}
int CrossplotSamples::nearest(const PlotFrame &f, QPointF point,
                              double radius) {
  double best = radius * radius;
  int found = -1;
  for (const auto &p : f.points) {
    const double dx = p.x - point.x(), dy = p.y - point.y(),
                 dist = dx * dx + dy * dy;
    if (dist <= best) {
      best = dist;
      found = p.sample;
    }
  }
  return found;
}
} // namespace paleo::crossplot
