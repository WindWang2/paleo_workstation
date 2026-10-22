// 层：QGIS 封装
#include "projectnativegrid.h"
#include "../catalog/datacatalog.h"
#include <qgscoordinatetransform.h>
#include <qgsdatumtransform.h>
#include <qgsexception.h>
#include <qgsproject.h>

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
    }
  } catch (const QgsCsException &e) { if (error) *error = e.what(); return false; }
  project->setTransformContext(context);
  project->setCrs(target);
  project->writeEntry("paleo", "localTransformTargets", targets);
  return true;
}
