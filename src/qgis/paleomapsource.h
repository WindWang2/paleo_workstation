// 层：QGIS 封装
#pragma once
#include <QVariantMap>
#include <QVector>
namespace PaleoMapSource {
// Inspect private providers; unreferenced pictures remain reference documents.
QVariantMap inspect(const QString &path, QString *error);
double surveyArea(const QVector<QVector<QPair<double, double>>> &polygons);
}
