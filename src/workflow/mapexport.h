#pragma once
#include <QString>

class DataCatalog;
class QgsPrintLayout;
class QgisLayerService;
class QgisProjectService;

// workflow/mapexport — 阶段C 布局导出（PROJECT_AREA_PLAN §5C §162/§215/§233）
// + 阶段E 产物登记。
//
// 一张 <horizon> 厚度图：QgsPrintLayout（A4 横版）+ 地图项（井位
// wells.thickness.<h> 在最上，相多边形 facies.<h> 仅在跑过相编码多边形化
// 之后才加入，厚度栅格 factor.<h>.idw 垫底）+ 标题「<h> 厚度」+ 井名
// 标注 + 米制图例 + 比例尺 + 指北针 + CRS 说明「工程坐标 · 米 · 未投影」；
// 栅格 nodata（-9999）不画。导出走既有 PaleoLayoutExportActions::exportLayout
// (Pdf) 管线。头文件只出现 Paleo/Qt 类型名（§25 纪律），Qgs* 实现留在 .cpp；
// QgsPrintLayout 仅以不透明指针穿过接口（测试检查图件项用）。
//
// 返回导出文件路径（失败为空 + *error）。要求 factor.<h>.idw 已声明（先跑
// MappingWorkflow::runThicknessChain）。布局挂在 QgisProjectService 的工程上
// ——不是 QgsProject::instance() 单例（canvas 工作已证实两者不是同一个）。
// 发布门（阶段E）以本导出成功为前提。
QString exportHorizonMapPdf( QgisLayerService *layers, QgisProjectService *projectSvc,
                             const QString &horizon, const QString &outPath,
                             QString *error = nullptr );

// 构建厚度图打印布局（导出与测试共用这条路径）。返回堆对象，调用方负责
// delete；失败 → nullptr + *error。
QgsPrintLayout *buildHorizonMapLayout( QgisLayerService *layers, QgisProjectService *projectSvc,
                                       const QString &horizon, QString *error = nullptr );

// 阶段E — 导出产物登记：把已写出的 PDF 复制进 catalog 受管 OUTPUT 区
// （artifacts/output/<asset_id>/<version_id>/<filename>，落盘只读），登记
// asset + version；SHA-256 先算（dedup 与登记共用同一份摘要）。同 SHA-256 已在库 → 复用
// 既有版本的资产 id（§3 dedup），不新增资产。版本不挂父（导出物不是任何
// 版本的父）。
// catalog 为空 / projectDir 为空 / 复制失败 → 返回空 assetId + *error。
// sha256Out 带出文件摘要；managedPathOut 带出受管副本的绝对路径（发布门
// 登记与快照都应指向受管文件，而不是用户随手挪动的原件）。
QString registerMapPdfAsset( DataCatalog *catalog, const QString &projectDir,
                             const QString &pdfPath, QString *sha256Out = nullptr,
                             QString *managedPathOut = nullptr, QString *error = nullptr );
