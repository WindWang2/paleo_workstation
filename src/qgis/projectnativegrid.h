// 层：QGIS 封装
#pragma once
#include <QStringList>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransformcontext.h>

namespace paleo::mapreference {
struct PreparedProjectGrid {
  QgsCoordinateTransformContext context;
  QgsCoordinateReferenceSystem crs;
  QStringList targets;
};

bool prepareNativeProjectGrid(PreparedProjectGrid &grid, const QString &sourceCrs,
                              const QString &destination, QString *error);
}
