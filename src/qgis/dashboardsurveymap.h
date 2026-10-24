// 层：QGIS 封装
#pragma once

// PW-004：看板「工区地图」的 QGIS 面全部收进 src/qgis（#689 把井位内存层 /
// 工区范围 rubberband / 漫游工具直接写进 ui/datapreview/projectstatisticspage，
// 新增 qgs 直连与 QtWidgets 传递边，lint 红）。本头只前向声明 + 值类型，
// 视图层 include 不带入 qgs* 或 <QWidget>。
#include <QColor>
#include <QFont>
#include <QString>
#include <QVector>

class QWidget;
struct CatalogEntity;

namespace DashboardSurveyMap
{
  struct Style
  {
    QColor primary;          // 井位/工区范围主色
    QFont labelFont;         // 井名标注字体
    double labelPt = 9.0;    // 井名标注字号（pt）
    QString wellsLayerName;  // 井位层显示名（已 tr）
  };

  // 新建预览画布（PreviewMapCanvas）+ 漫游工具 + 井位内存层（objectName
  // dashboardWellsLayer）+ 工区范围 rubberband（dashboardSurveyRubberBand）。
  // 所有权归 parent 父子树。
  QWidget *create( QWidget *parent, const Style &style );

  // create() 返回值内嵌的 QgsMapCanvas（作为 QWidget）；用于挂装饰。
  QWidget *canvasWidget( QWidget *map );

  // 先同步 stopRendering 再重灌井位层（#693 堆损坏纪律），按
  // 角点 → inline/xline → 井位并集 → 10 km 默认 取工区范围并缩放到全图。
  void update( QWidget *map, const QVector<CatalogEntity> &wells, const CatalogEntity *survey );
}
