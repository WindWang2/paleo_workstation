// 层：QGIS 封装
#pragma once
#include <QVariantMap>
#include <QVector>
#include <functional>
namespace PaleoMapSource {
// Inspect private providers; unreferenced pictures remain reference documents.
QVariantMap inspect(const QString &path, QString *error);
// Empty result with no error means this is not a recognized microfacies polygon.
QVariantMap normalizeFacies(const QString &source, const QString &output, const QVariantList &schema,
                           const QString &microField, const std::function<bool()> &cancelled, QString *error);
double surveyArea(const QVector<QVector<QPair<double, double>>> &polygons);
}
