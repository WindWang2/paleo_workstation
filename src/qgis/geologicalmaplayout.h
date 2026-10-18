// 层：QGIS 封装
#pragma once
#include "standardelements.h"
#include <QVariantMap>
class QgsRectangle;
namespace PaleoGeologicalMapLayout {
struct Settings {
  QString horizon, title, subtitle, author, reviewer;
  int version = 0;
  double rotation = 0, scale = 0;
  PaleoStandardElements::FigureKind kind = PaleoStandardElements::FigureKind::Facies;
  PaleoStandardElements::PageSetup page;
};
QgsPrintLayout *build(QgsProject *project, const QList<QgsMapLayer *> &layers,
    const QgsRectangle &extent, const Settings &settings, QString *error = nullptr);
QVariantMap describe(QgsPrintLayout *layout);
// Run after rendering: QGIS legends may resize to their actual content then.
bool validateBounds(QgsPrintLayout *layout, QString *error = nullptr);
}
