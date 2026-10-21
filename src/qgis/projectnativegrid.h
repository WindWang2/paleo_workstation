// 层：QGIS 封装
#pragma once
#include <QString>
class QgsProject;
bool configureNativeProjectGrid(QgsProject *project, const QString &sourceCrs, const QString &destination, QString *error);
