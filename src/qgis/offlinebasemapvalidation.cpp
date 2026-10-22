// 层：QGIS 封装
#include "offlinebasemapvalidation.h"
#include "projectmapreference.h"
#include <qgsrasterlayer.h>
#include <QFileInfo>
#include <QObject>
#include <memory>
namespace PaleoOfflineRaster {
QString validate(const QString &path) {
  const QStringList supported{"mbtiles", "tif", "tiff", "img", "gpkg", "png", "jpg", "jpeg"};
  if (!supported.contains(QFileInfo(path).suffix().toLower()))
    return QObject::tr("请选择 MBTiles、GeoTIFF、IMG、栅格 GeoPackage 或已配准图片；VRT 等外部引用数据请先导出为 GeoTIFF。");
  std::unique_ptr<QgsRasterLayer> layer(paleo::mapreference::offlineBasemap(path, QString()));
  if (!layer) return QObject::tr("文件不是可读取的栅格底图，或缺少配套文件。");
  if (!layer->crs().isValid()) return QObject::tr("底图缺少坐标系，请补齐投影／配准信息后再导入。");
  if (layer->extent().isEmpty()) return QObject::tr("底图没有有效的空间范围。");
  return {};
}
}
