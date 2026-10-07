// 层：QGIS 封装
#include "projectmapreference.h"
#include "catalog/datacatalog.h"
#include "qgiserrors_internal.h"
#include <QFileInfo>
#include <QUrl>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <cmath>
#include <qgscoordinatetransform.h>
#include <qgsdatumtransform.h>
#include <qgsexception.h>
#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>

namespace paleo::mapreference {
namespace {
QString affine(const PaleoGeoreference &g)
{
  // affine 保持线性单位：先输出等距圆柱投影米坐标，再由原生逆投影转弧度。
  // 直接 affine → 度会把角度单位传播到工程坐标输入，导致 PROJ 范围转换失败。
  const auto number = [](double d) { return QString::number(d, 'g', 17); };
  const double factor = 6378137.0 * std::acos(-1.0) / 180.0;
  return QStringLiteral("+proj=pipeline +step +proj=affine +s11=%1 +s12=%2 +s21=%3 +s22=%4 +xoff=%5 +yoff=%6 +step +inv +proj=eqc +R=6378137")
      .arg(number(factor * g.a / g.metersPerDegLon), number(-factor * g.b / g.metersPerDegLon),
           number(factor * g.b / g.metersPerDegLat), number(factor * g.a / g.metersPerDegLat),
           number(factor * (g.anchorLonDeg + g.tE / g.metersPerDegLon)),
           number(factor * (g.anchorLatDeg + g.tN / g.metersPerDegLat)));
}

QString tileAttribution(const QString &path)
{
  const auto connection = QStringLiteral("paleo-basemap-%1").arg(QUuid::createUuid().toString());
  QString attribution;
  {
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(path);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (database.open()) {
      QSqlQuery query(database);
      if (query.exec(QStringLiteral("SELECT value FROM metadata WHERE name='attribution'")) && query.next())
        attribution = query.value(0).toString();
    }
    database.close();
  }
  QSqlDatabase::removeDatabase(connection);
  return attribution;
}
}

bool configure(QgsProject *project, const std::optional<PaleoGeoreference> &reference,
               const QString &mapCrs, QString *error)
{
  if (!project) return false;
  const auto local = QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  auto context = project->transformContext();
  // 删除上一份配准的操作，保留工程其它 CRS 的原生转换设置。
  QStringList targets = project->readListEntry("paleo", "localTransformTargets");
  targets << QStringLiteral("EPSG:4326") << QStringLiteral("EPSG:3857");
  for (const auto &target : targets)
    context.removeCoordinateOperation(local, QgsCoordinateReferenceSystem(target));
  project->setCrs(local);
  project->setTransformContext(context);
  project->removeEntry("paleo", "localTransformTargets");
  if (!reference) return true;
  const auto fail = [&](const QString &message) {
    paleo::qgis_detail::setError(error, message);
    return false;
  };
  if (!reference->isComplete())
    return fail(QObject::tr("工程坐标配准参数无效"));
  const QgsCoordinateReferenceSystem destination(mapCrs);
  if (!destination.isValid() || destination == local || destination.type() == Qgis::CrsType::Engineering)
    return fail(QObject::tr("地图坐标系无效或不是地理/投影坐标系：%1").arg(mapCrs));
  const QgsCoordinateReferenceSystem geographic(QStringLiteral("EPSG:4326"));
  const QString prefix = affine(*reference);
  targets = {QStringLiteral("EPSG:4326"), QStringLiteral("EPSG:3857"), mapCrs};
  targets.removeDuplicates();
  try {
    for (const auto &target : targets) {
      const QgsCoordinateReferenceSystem crs(target);
      QString operation = crs == geographic
          ? prefix + QStringLiteral(" +step +proj=unitconvert +xy_in=rad +xy_out=deg") : prefix;
      if (crs != geographic) {
        // QGIS 返回的 operation 已按可视化规范化（经度/纬度及度单位）。
        QgsCoordinateTransform projection(geographic, crs, context);
        projection.setAllowFallbackTransforms(false);
        projection.transform(QgsPointXY(reference->anchorLonDeg, reference->anchorLatDeg));
        QString tail = projection.instantiatedCoordinateOperationDetails().proj.trimmed();
        if (tail.isEmpty()) return fail(QObject::tr("无法生成地图投影操作：%1").arg(target));
        if (crs.isGeographic() && tail == QLatin1String("+proj=noop")) {
          operation = prefix + QStringLiteral(" +step +proj=unitconvert +xy_in=rad +xy_out=deg");
          tail.clear();
        } else if (tail.startsWith(QLatin1String("+proj=pipeline")))
          tail = tail.mid(QStringLiteral("+proj=pipeline").size()).trimmed();
        else
          tail.prepend(QStringLiteral("+step "));
        // 上一段逆投影已经输出弧度，去掉规范化操作的首个度→弧度步骤。
        const QString degreesToRadians = QStringLiteral("+step +proj=unitconvert +xy_in=deg +xy_out=rad");
        if (tail.startsWith(degreesToRadians))
          tail = tail.mid(degreesToRadians.size()).trimmed();
        if (!tail.isEmpty()) operation += QLatin1Char(' ') + tail;
      }
      if (!context.addCoordinateOperation(local, crs, operation, false))
        return fail(QObject::tr("无法注册工程坐标转换：%1").arg(target));
      QgsCoordinateTransform transform(local, crs, context);
      if (!transform.isValid()) return fail(QObject::tr("工程坐标转换不能使用：%1").arg(target));
      const auto point = transform.transform(QgsPointXY(0, 0));
      if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
        return fail(QObject::tr("工程坐标转换返回无效位置：%1").arg(target));
      // 地图绘制还会调用原生范围转换；保存配置前一并确认其正反向可用。
      const auto bounds = transform.transformBoundingBox(QgsRectangle(0, 0, 1000, 1000));
      const auto inverseBounds = transform.transformBoundingBox(bounds, Qgis::TransformDirection::Reverse);
      const auto roundTrip = transform.transform(point, Qgis::TransformDirection::Reverse);
      if (!std::isfinite(bounds.xMinimum()) || !std::isfinite(bounds.yMinimum())
          || !std::isfinite(bounds.xMaximum()) || !std::isfinite(bounds.yMaximum())
          || !std::isfinite(inverseBounds.xMinimum()) || !std::isfinite(inverseBounds.yMinimum())
          || std::hypot(roundTrip.x(), roundTrip.y()) > 0.001)
        return fail(QObject::tr("工程坐标范围转换不可用：%1").arg(target));
    }
  } catch (const QgsCsException &ex) {
    return fail(ex.what());
  }
  project->setTransformContext(context);
  project->setCrs(destination);
  project->writeEntry("paleo", "localTransformTargets", targets);
  return true;
}

bool transformPoint(QgsMapCanvas *canvas, const QgsPointXY &point,
                    const QgsCoordinateReferenceSystem &source,
                    const QgsCoordinateReferenceSystem &destination, QgsPointXY *result)
{
  if (!canvas || !source.isValid() || !destination.isValid() || !result) return false;
  try {
    QgsCoordinateTransform transform(source, destination, canvas->mapSettings().transformContext());
    if (!transform.isValid()) return false;
    *result = transform.transform(point);
    return std::isfinite(result->x()) && std::isfinite(result->y());
  } catch (const QgsCsException &) { return false; }
}

QgsRectangle mapExtent(QgsMapCanvas *canvas, const QgsRectangle &extent,
                       const QgsCoordinateReferenceSystem &source)
{
  if (!canvas || !source.isValid() || extent.isNull()) return {};
  try {
    QgsCoordinateTransform transform(source, canvas->mapSettings().destinationCrs(),
                                     canvas->mapSettings().transformContext());
    if (!transform.isValid()) return {};
    return transform.transformBoundingBox(extent);
  } catch (const QgsCsException &) { return {}; }
}

QString mbtilesUri(const QString &path)
{
  return QStringLiteral("type=mbtiles&url=%1").arg(QString::fromLatin1(
      QUrl::toPercentEncoding(QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString())));
}

QgsRasterLayer *offlineBasemap(const QString &path, const QString &title, QObject *parent)
{
  if (!QFileInfo(path).isFile()) return nullptr;
  auto *layer = new QgsRasterLayer(mbtilesUri(path), title, QStringLiteral("wms"));
  if (!layer->isValid()) { delete layer; return nullptr; }
  layer->setParent(parent);
  layer->setCustomProperty("paleoBasemap", true);
  layer->setCustomProperty("paleoBasemapPath", QFileInfo(path).absoluteFilePath());
  layer->setCustomProperty("paleoBasemapAttribution", tileAttribution(path));
  return layer;
}

QString attribution(QgsMapCanvas *canvas)
{
  QStringList sources;
  if (canvas) for (auto *layer : canvas->layers()) {
    const auto source = layer->customProperty("paleoBasemapAttribution").toString();
    if (!source.isEmpty() && !sources.contains(source)) sources << source;
  }
  return sources.join(QStringLiteral(" · "));
}
}
