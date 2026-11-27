// 层：QGIS 封装
#include "seismicclusterpatches.h"
#include <QFile>
#include <QObject>
#include <QScopeGuard>
#include <algorithm>
#include <cmath>
#include <memory>
#include <gdal_priv.h>
#include <gdal_alg.h>
#include <ogrsf_frmts.h>

namespace {
bool fail(QString *error, const QString &reason) {
  if (error) *error = reason;
  return false;
}
struct Grid {
  int columns, rows;
  double affine[6]{};
};
struct Hooks {
  const std::function<bool()> &cancel;
  const std::function<void(double)> &progress;
};
int CPL_STDCALL feedback(double fraction, const char *, void *user) {
  const auto &hooks = *static_cast<Hooks *>(user);
  if (hooks.progress) hooks.progress(85.0 * fraction);
  return !(hooks.cancel && hooks.cancel());
}
bool addField(OGRLayer *layer, const char *name, OGRFieldType type) {
  OGRFieldDefn field(name, type);
  return layer->CreateField(&field) == OGRERR_NONE;
}
void addGridRing(OGRPolygon &polygon, const OGRLinearRing &source, const Grid &grid) {
  // GDAL drops collinear vertices independently per polygon. Insert the common cell
  // corners so a long edge exactly matches neighbouring short edges in QGIS coverage editing.
  OGRLinearRing ring;
  for (int i = 1; i < source.getNumPoints(); ++i) {
    const qint64 x = qRound64((source.getX(i - 1) - grid.affine[0]) / grid.affine[1]);
    const qint64 y = qRound64((source.getY(i - 1) - grid.affine[3]) / grid.affine[5]);
    const qint64 dx = qRound64((source.getX(i) - source.getX(i - 1)) / grid.affine[1]);
    const qint64 dy = qRound64((source.getY(i) - source.getY(i - 1)) / grid.affine[5]);
    const qint64 length = std::max(std::abs(dx), std::abs(dy));
    for (qint64 step = 0; step < length; ++step)
      ring.addPoint(grid.affine[0] + (x + step * (dx / length)) * grid.affine[1],
                    grid.affine[3] + (y + step * (dy / length)) * grid.affine[5]);
  }
  ring.closeRings();
  polygon.addRing(&ring);
}

bool identify(OGRFeature *feature, const Grid &grid, QString *error) {
  const auto *geometry = feature->GetGeometryRef();
  if (!geometry || wkbFlatten(geometry->getGeometryType()) != wkbPolygon)
    return fail(error, QObject::tr("聚类图斑几何无效"));
  const auto *polygon = geometry->toPolygon();
  const auto *ring = polygon->getExteriorRing();
  if (!ring || ring->getNumPoints() < 4)
    return fail(error, QObject::tr("聚类图斑外环无效"));
  // The leftmost top edge starts at the first occupied cell in row-major order.
  qint64 first = qint64(grid.rows) * grid.columns;
  for (int i = 1; i < ring->getNumPoints(); ++i) {
    const auto gridRow = [&](int p) { return qRound64((ring->getY(p) - grid.affine[3]) / grid.affine[5]); };
    const qint64 row = gridRow(i);
    if (row != gridRow(i - 1) || row >= grid.rows) continue;
    const double edgeX = std::min(ring->getX(i - 1), ring->getX(i));
    const qint64 col = qRound64((edgeX - grid.affine[0]) / grid.affine[1]);
    first = std::min(first, row * grid.columns + col);
  }
  const qint64 pixels = qRound64(polygon->get_Area() / std::abs(grid.affine[1] * grid.affine[5]));
  if (first < 0 || first >= qint64(grid.rows) * grid.columns || pixels < 1 ||
      feature->GetFieldAsInteger("cluster_id") < 1)
    return fail(error, QObject::tr("聚类图斑的网格编号或覆盖无效"));
  feature->SetField("patch_id", static_cast<GIntBig>(first));
  feature->SetField("pixel_count", static_cast<GIntBig>(pixels));
  OGRPolygon noded;
  addGridRing(noded, *ring, grid);
  for (int i = 0; i < polygon->getNumInteriorRings(); ++i)
    addGridRing(noded, *polygon->getInteriorRing(i), grid);
  if (feature->SetGeometry(&noded) != OGRERR_NONE)
    return fail(error, QObject::tr("无法保存聚类图斑的格网边界"));
  return true;
}
}

bool SeismicClusterPatches::polygonize(const QString &rasterPath, const QString &outputPath,
                                      const QString &crsWkt, QString *error,
                                      const std::function<bool()> &cancel,
                                      const std::function<void(double)> &progress) {
  GDALAllRegister();
  const auto close = [](GDALDataset *ds) { GDALClose(ds); };
  using Dataset = std::unique_ptr<GDALDataset, decltype(close)>;
  Dataset src(GDALDataset::Open(rasterPath.toUtf8().constData(), GDAL_OF_RASTER), close);
  if (!src || !src->GetRasterBand(1)) return fail(error, QObject::tr("无法打开原始聚类栅格"));
  Grid grid{src->GetRasterXSize(), src->GetRasterYSize()};
  if (src->GetGeoTransform(grid.affine) != CE_None || grid.affine[1] <= 0 ||
      grid.affine[5] >= 0 || grid.affine[2] != 0 || grid.affine[4] != 0)
    return fail(error, QObject::tr("聚类图斑需要有效的北向上网格"));
  auto *driver = GetGDALDriverManager()->GetDriverByName("GPKG");
  if (!driver || QFile::exists(outputPath)) return fail(error, QObject::tr("聚类面输出驱动或路径不可用"));
  bool saved = false;
  const auto cleanup = qScopeGuard([&] { if (!saved) QFile::remove(outputPath); });
  Dataset out(driver->Create(outputPath.toUtf8().constData(), 0, 0, 0, GDT_Unknown, nullptr), close);
  if (!out) return fail(error, QObject::tr("无法创建聚类面文件"));
  OGRSpatialReference crs;
  if (!crsWkt.isEmpty() && crs.SetFromUserInput(crsWkt.toUtf8().constData()) != OGRERR_NONE)
    return fail(error, QObject::tr("聚类栅格坐标系无效"));
  auto *layer = out->CreateLayer("cluster_patches", crsWkt.isEmpty() ? src->GetSpatialRef() : &crs, wkbPolygon);
  if (!layer || !addField(layer, "cluster_id", OFTInteger) || !addField(layer, "patch_id", OFTInteger64) ||
      !addField(layer, "pixel_count", OFTInteger64) || layer->StartTransaction() != OGRERR_NONE)
    return fail(error, QObject::tr("无法创建聚类面属性"));
  auto *band = src->GetRasterBand(1);
  Hooks hooks{cancel, progress};
  if (GDALPolygonize(band, band->GetMaskBand(), layer, 0, nullptr, feedback, &hooks) != CE_None)
    return fail(error, cancel && cancel() ? QObject::tr("已取消") : QObject::tr("原始聚类栅格转面失败"));
  layer->ResetReading();
  for (auto &&feature : *layer) {
    if (cancel && cancel()) return fail(error, QObject::tr("已取消"));
    if (!identify(feature.get(), grid, error)) return false;
    if (layer->SetFeature(feature.get()) != OGRERR_NONE) return fail(error, QObject::tr("写入聚类面编号失败"));
  }
  if (layer->CommitTransaction() != OGRERR_NONE || out->FlushCache() != CE_None)
    return fail(error, QObject::tr("提交聚类面失败"));
  saved = true;
  return true;
}
