#pragma once
#include <QString>

class QgisLayerService;

// workflow/mapexport — 阶段C 布局导出（PROJECT_AREA_PLAN §5C）。
// 一张含井位 + 相多边形的 <horizon> 图：QgsPrintLayout（A4 横版）+ 地图
// 项（facies.<h> + wells.thickness.<h> 实例化图层）+ 标题，走现有
// PaleoLayoutExportActions::exportLayout(Pdf) 管线。头文件只出现 Qt 类型
//（§25 纪律），Qgs* 全部留在 .cpp。
//
// 返回导出文件路径（失败为空 + *error）。要求 facies.<h> 已声明（先跑
// MappingWorkflow::runThicknessChain）。发布门（阶段E）以本导出成功为前提。
QString exportHorizonMapPdf( QgisLayerService *layers, const QString &horizon,
                             const QString &outPath, QString *error = nullptr );
