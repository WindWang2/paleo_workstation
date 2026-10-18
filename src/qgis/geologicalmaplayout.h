// 层：QGIS 封装
#pragma once
#include "layoutexport.h"
#include "standardelements.h"
#include <QVariantMap>
#include <memory>
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
// build() 的句柄化入口（#531）：把版面装进 PaleoLayoutExport::LayoutHandle 交给
// 对话框 Builder / 导出入口——视图层与功能层只见句柄，不见 QgsPrintLayout。
// build 失败 → nullptr（不交出包着空版面的句柄）。
std::unique_ptr<PaleoLayoutExport::LayoutHandle> buildHandle(
    QgsProject *project, const QList<QgsMapLayer *> &layers, const QgsRectangle &extent,
    const Settings &settings, QString *error = nullptr);
QVariantMap describe(QgsPrintLayout *layout);
// Run after rendering: QGIS legends may resize to their actual content then.
bool validateBounds(QgsPrintLayout *layout, QString *error = nullptr);
}
