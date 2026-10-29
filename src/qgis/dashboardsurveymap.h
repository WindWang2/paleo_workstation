// 层：QGIS 封装
#pragma once

// PW-004：看板「工区地图」的 QGIS 面全部收进 src/qgis（#689 把井位内存层 /
// 漫游工具直接写进 ui/datapreview/projectstatisticspage，新增 qgs 直连与
// QtWidgets 传递边，lint 红）。本头只前向声明 + 值类型，
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
    QColor primary;          // 井位主色
    QFont labelFont;         // 井名标注字体
    double labelPt = 9.0;    // 井名标注字号（pt）
    QString wellsLayerName;  // 井位层显示名（已 tr）
  };

  // 工区三视图平面范围（米）。数据服务 readProjectPlanarRange 的视图侧
  // 值形态（服务头不出闸）；无有效坐标时 valid=false。仅用于取景与
  // 范围量算——范围框按用户要求撤下，不在地图上绘制。
  struct RangeRect
  {
    bool valid = false;
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
  };
  struct AreaRanges
  {
    RangeRect wells;    // 测井范围：有井口坐标的井 → 最小外接矩形
    RangeRect seismic;  // 地震范围：可换算地震体 → 并集矩形
    RangeRect merged;   // 工区范围：两者并集；量级失配时无效
  };

  // 新建预览画布（PreviewMapCanvas）+ 漫游工具 + 井位内存层（objectName
  // dashboardWellsLayer）。所有权归 parent 父子树。
  QWidget *create( QWidget *parent, const Style &style );

  // create() 返回值内嵌的 QgsMapCanvas（作为 QWidget）；用于挂装饰。
  QWidget *canvasWidget( QWidget *map );

  // 先同步 stopRendering 再重灌井位层（#693 堆损坏纪律），按
  // ranges->merged → 角点 → 井位并集 → 10 km 默认 取景并缩放到全图；
  // 只画井位，不画范围框（工区范围带已按用户要求撤下）。
  void update( QWidget *map, const QVector<CatalogEntity> &wells, const CatalogEntity *survey,
               const AreaRanges *ranges = nullptr );
}
