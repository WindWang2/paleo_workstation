// 层：数据
#include "seismicattributefusionclient_internal.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QtEndian>
#include <cmath>
#include <limits>

namespace paleo::fusion_detail {
namespace {
bool close(double a, double b) {
  return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= 1e-6 + std::abs(b) * 1e-10;
}
bool integer(const QJsonValue &value, int minimum, int maximum, int &out) {
  const auto number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
  if (!std::isfinite(number) || number < minimum || number > maximum || std::floor(number) != number)
    return false;
  out = int(number);
  return true;
}

QString geometry(const QJsonObject &grid, qint64 maxCells, SeismicFusionResult &r)
{
  if (!integer(grid.value(QStringLiteral("columns")), 1, std::numeric_limits<int>::max(), r.columns) ||
      !integer(grid.value(QStringLiteral("rows")), 1, std::numeric_limits<int>::max(), r.rows) ||
      maxCells <= 0 || maxCells > 64 * 1024 * 1024 || qint64(r.columns) * r.rows > maxCells)
    return SeismicAttributeFusionClient::tr("Invalid grid dimensions or raster memory budget exceeded.");
  const auto number = [&](const char *key) {
    return grid.value(QLatin1String(key)).toDouble(std::numeric_limits<double>::quiet_NaN());
  };
  r.dx = number("dx");
  r.dy = number("dy");
  const double left = number("extent_left"), bottom = number("extent_bottom");
  const double width = number("width"), height = number("height");
  if (r.dx <= 0 || r.dy <= 0 || !close(width, r.columns * r.dx) || !close(height, r.rows * r.dy) ||
      !close(number("extent_right"), left + width) || !close(number("extent_top"), bottom + height))
    return SeismicAttributeFusionClient::tr("Grid spacing and outer pixel extent are inconsistent.");
  const auto transform = grid.value(QStringLiteral("gdal_geotransform")).toArray();
  const QVector<double> expected{left, r.dx, 0, bottom + height, 0, -r.dy};
  if (transform.size() != 6) return SeismicAttributeFusionClient::tr("Invalid north-up grid transform.");
  for (int i = 0; i < 6; ++i) {
    if (!close(transform[i].toDouble(std::numeric_limits<double>::quiet_NaN()), expected[i]))
      return SeismicAttributeFusionClient::tr("Invalid north-up grid transform.");
  }
  if (grid.value(QStringLiteral("binary_row_order")) != QLatin1String("north_to_south") ||
      grid.value(QStringLiteral("axis_order")) != QLatin1String("east_north_xy") ||
      grid.value(QStringLiteral("coordinate_unit")) != QLatin1String("m"))
    return SeismicAttributeFusionClient::tr("Unsupported raster row order, axes or coordinate unit.");
  r.extent = QRectF(left, bottom, width, height);
  r.gdalGeoTransform = expected;
  // API 1.0 places CRS in grid, not in a top-level crs object.
  r.crsAuth = grid.value(QStringLiteral("crs_auth")).toString();
  r.crsWkt = grid.value(QStringLiteral("crs_wkt")).toString();
  return {};
}

QString raster(SeismicFusionResult &r, int clusters, Control *control)
{
  QFile file(r.binPath);
  const qint64 cells = qint64(r.columns) * r.rows;
  if (!file.open(QIODevice::ReadOnly) || file.size() != cells * 4)
    return SeismicAttributeFusionClient::tr("Raster binary is unreadable or has the wrong size: %1").arg(r.binPath);
  r.cellsNorthUp.resize(cells);
  qint64 index = 0;
  int valid = 0;
  while (index < cells) {
    if (control) {
      const auto error = control->interruption();
      if (!error.isEmpty()) return error;
    }
    const auto bytes = file.read(std::min(qint64(64 * 1024), (cells - index) * 4));
    if (bytes.isEmpty() || bytes.size() % 4) return SeismicAttributeFusionClient::tr("Raster binary was truncated while reading.");
    for (qsizetype offset = 0; offset < bytes.size(); offset += 4) {
      const int code = qFromLittleEndian<qint32>(bytes.constData() + offset);
      if (code != -9999 && (code < 1 || code > clusters)) return SeismicAttributeFusionClient::tr("Raster contains an invalid cluster code.");
      if (code != -9999) ++valid;
      r.cellsNorthUp[index++] = code;
    }
  }
  if (valid != r.validCells || file.size() != cells * 4)
    return SeismicAttributeFusionClient::tr("Raster data and classification counts are inconsistent.");
  return {};
}
} // namespace

SeismicFusionResult parse(const QJsonObject &root, const QString &fallback, qint64 maxCells, Control *control)
{
  if (control && !control->interruption().isEmpty()) return failure(control->interruption());
  const auto state = root.value(QStringLiteral("state")).toString();
  if ((!state.isEmpty() && state != QLatin1String("succeeded")) ||
      (root.contains(QStringLiteral("result")) && !root.value(QStringLiteral("result")).isObject())) {
    auto error = root.value(QStringLiteral("error")).toString();
    return failure(error.isEmpty() ? SeismicAttributeFusionClient::tr("Service job did not succeed.") : error);
  }
  const auto object = root.contains(QStringLiteral("result")) ? root.value(QStringLiteral("result")).toObject() : root;
  if (object.value(QStringLiteral("status")) != QLatin1String("succeeded")) {
    auto error = object.value(QStringLiteral("error")).toString();
    return failure(error.isEmpty() ? SeismicAttributeFusionClient::tr("Service result did not succeed.") : error);
  }
  SeismicFusionResult result;
  auto error = geometry(object.value(QStringLiteral("grid")).toObject(), maxCells, result);
  if (!error.isEmpty()) return failure(error);
  int clusters = 0;
  const auto classification = object.value(QStringLiteral("classification")).toObject();
  if (!integer(classification.value(QStringLiteral("n_clusters")), 2, 32, clusters) ||
      !integer(classification.value(QStringLiteral("valid_cells")), 0, result.columns * result.rows, result.validCells) ||
      object.value(QStringLiteral("nodata")).toInt(0) != -9999)
    return failure(SeismicAttributeFusionClient::tr("Invalid cluster statistics or nodata value."));
  result.unitName = object.value(QStringLiteral("unit_name")).toString();
  auto output = object.value(QStringLiteral("output_dir")).toString();
  if (output.isEmpty()) output = fallback;
  if (output.isEmpty()) return failure(SeismicAttributeFusionClient::tr("Service result has no output directory."));
  result.outputDir = QFileInfo(output).absoluteFilePath();
  const auto files = object.value(QStringLiteral("output_files")).toObject();
  const auto path = [&](const char *key) {
    const auto value = files.value(QLatin1String(key)).toString();
    return value.isEmpty() ? QString() : QDir(result.outputDir).absoluteFilePath(value);
  };
  result.geoTiffPath = path("geotiff_north_up");
  result.binPath = path("int32_north_up_bin");
  result.npzPath = path("facies_npz");
  result.cps3Path = path("cps3");
  result.manifestPath = path("manifest_json");
  if (result.geoTiffPath.isEmpty() || result.binPath.isEmpty())
    return failure(SeismicAttributeFusionClient::tr("Service result is missing required raster files."));
  for (const auto &file : {result.geoTiffPath, result.binPath, result.npzPath, result.cps3Path, result.manifestPath}) {
    if (!file.isEmpty() && (!QFileInfo(file).isFile() || !QFileInfo(file).isReadable()))
      return failure(SeismicAttributeFusionClient::tr("Result file is not readable: %1").arg(file));
  }
  error = raster(result, clusters, control);
  if (!error.isEmpty()) return failure(error);
  for (const auto &attribute : classification.value(QStringLiteral("selected_attributes")).toArray())
    result.selectedAttributes.append(attribute.toString());
  result.clusterSummary = classification.value(QStringLiteral("cluster_summary")).toArray().toVariantList();
  result.wellPredictions = object.value(QStringLiteral("well_predictions")).toArray().toVariantList();
  result.elapsedSeconds = object.value(QStringLiteral("elapsed_seconds")).toDouble();
  if (control && !control->interruption().isEmpty()) return failure(control->interruption());
  result.ok = true;
  return result;
}
} // namespace paleo::fusion_detail
