// 层：QGIS 封装
#include "projectnativegrid.h"
#include "../catalog/datacatalog.h"
#include <cmath>
#include <qgscoordinatetransform.h>
#include <qgsdatumtransform.h>
#include <qgsexception.h>
#include <qgsproject.h>
#include <qgsrectangle.h>

bool configureNativeProjectGrid(QgsProject *project, const QString &sourceCrs, const QString &destination, QString *error)
{
  const QgsCoordinateReferenceSystem source(sourceCrs), target(destination);
  const auto local = QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
  if (!source.isValid() || !target.isValid()) { if (error) *error = QObject::tr("工程或地图坐标系无效"); return false; }
  auto context = project->transformContext();
  QStringList targets{QStringLiteral("EPSG:4326"), QStringLiteral("EPSG:3857"), destination, sourceCrs};
  targets.removeDuplicates();
  try {
    for (const auto &id : targets) {
      const QgsCoordinateReferenceSystem crs(id);
      QString operation = QStringLiteral("+proj=noop");
      if (source != crs) {
        QgsCoordinateTransform native(source, crs, context);
        if (!native.isValid()) { if (error) *error = QObject::tr("无法建立原生坐标转换：%1").arg(id); return false; }
        native.setAllowFallbackTransforms(false);
        native.transform(QgsPointXY(0, 0));
        operation = native.instantiatedCoordinateOperationDetails().proj;
        if (operation.isEmpty()) { if (error) *error = QObject::tr("无法建立原生坐标转换：%1").arg(id); return false; }
      }
      // Geological algorithms retain one engineering grid; its numeric values
      // are already in sourceCrs. Bind an exact native operation, no affine fit.
      if (!context.addCoordinateOperation(local, crs, operation, false)) {
        if (error) *error = QObject::tr("无法注册工程坐标转换：%1").arg(id);
        return false;
      }
      // 注册后同款可用性校验（affine 路径 projectmapreference.cpp:112-125 的
      // 口径）：只验证实例化不够——坏操作串要到渲染期才静默出现，而工程此刻
      // 已把坏上下文存盘。保存前确认点/边界/往返都真的能算。
      QgsCoordinateTransform transform(local, crs, context);
      if (!transform.isValid()) { if (error) *error = QObject::tr("工程坐标转换不能使用：%1").arg(id); return false; }
      const auto point = transform.transform(QgsPointXY(0, 0));
      if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
      { if (error) *error = QObject::tr("工程坐标转换返回无效位置：%1").arg(id); return false; }
      // 地图绘制还会调用原生范围转换；保存配置前一并确认其正反向可用。
      const auto bounds = transform.transformBoundingBox(QgsRectangle(0, 0, 1000, 1000));
      const auto inverseBounds = transform.transformBoundingBox(bounds, Qgis::TransformDirection::Reverse);
      const auto roundTrip = transform.transform(point, Qgis::TransformDirection::Reverse);
      if (!std::isfinite(bounds.xMinimum()) || !std::isfinite(bounds.yMinimum())
          || !std::isfinite(bounds.xMaximum()) || !std::isfinite(bounds.yMaximum())
          || !std::isfinite(inverseBounds.xMinimum()) || !std::isfinite(inverseBounds.yMinimum())
          || std::hypot(roundTrip.x(), roundTrip.y()) > 0.001)
      { if (error) *error = QObject::tr("工程坐标范围转换不可用：%1").arg(id); return false; }
    }
  } catch (const QgsCsException &e) { if (error) *error = e.what(); return false; }
  project->setTransformContext(context);
  project->setCrs(target);
  project->writeEntry("paleo", "localTransformTargets", targets);
  return true;
}
