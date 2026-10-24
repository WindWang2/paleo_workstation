// 层：视图
#pragma once
#include <QColor>
#include <QIcon>
#include <QString>
namespace PaleoTheme {
QIcon graphIcon(const QString &name);
QColor graphColor(int category);
QColor graphCanvasColor();
QColor graphInkColor();
QString graphSurfaceStyleSheet();
}
