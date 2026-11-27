// 层：QGIS 封装
#include "clusterassignmentgrid.h"
#include "qgisprojectservice.h"
#include <qgsproject.h>
#include <gdal.h>
#include <QObject>
#include <limits>
#include <algorithm>

std::shared_ptr<const QgsCoordinateTransformContext> clusterTransformContext(QgisProjectService *project) {
  return std::make_shared<QgsCoordinateTransformContext>(project->project()->transformContext());
}

SeismicClusterResult readClusterAssignmentGrid(const QString &file, const std::function<bool()> &cancel) {
  SeismicClusterResult grid;
  std::unique_ptr<void, decltype(&GDALClose)> ds(GDALOpen(file.toUtf8().constData(), GA_ReadOnly), GDALClose);
  double gt[6];
  if (!ds || GDALGetRasterCount(ds.get()) != 1 || GDALGetGeoTransform(ds.get(), gt) != CE_None ||
      gt[1] <= 0 || gt[5] >= 0 || gt[2] != 0 || gt[4] != 0) {
    grid.error = QObject::tr("原始簇图缺少北向上的分类网格"); return grid;
  }
  grid.rows = GDALGetRasterYSize(ds.get()); grid.columns = GDALGetRasterXSize(ds.get());
  const qint64 count = qint64(grid.rows) * grid.columns;
  if (count <= 0 || count > std::numeric_limits<int>::max()) {
    grid.error = QObject::tr("原始簇图尺寸无效"); return grid;
  }
  grid.extent = QRectF(gt[0], gt[3] + gt[5] * grid.rows, gt[1] * grid.columns, -gt[5] * grid.rows);
  const auto canonical = GDALGetMetadataItem(ds.get(),"PALEO_CRS_WKT",nullptr);
  grid.crsWkt = QString::fromUtf8(canonical && *canonical ? canonical : GDALGetProjectionRef(ds.get()));
  if(grid.crsWkt.isEmpty()) { grid.error=QObject::tr("原始簇图缺少坐标系"); return grid; }
  auto band = GDALGetRasterBand(ds.get(), 1);
  grid.cells.resize(count);
  for (int y = 0; y < grid.rows;) {
    if (cancel && cancel()) { grid.error = QObject::tr("已取消"); break; }
    const int height = std::min(128, grid.rows - y);
    if (GDALRasterIO(band, GF_Read, 0, y, grid.columns, height, grid.cells.data() + qint64(y)*grid.columns,
                     grid.columns, height, GDT_Int32, 0, 0) != CE_None) {
      grid.error = QObject::tr("无法读取原始簇栅格"); break;
    }
    y += height;
  }
  return grid;
}
