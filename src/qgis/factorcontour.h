// 层：QGIS 封装
#pragma once
#include <QString>
#include <QVector>
#include <functional>
#include <memory>
class PaleoTaskService;
class QObject;

// qgis/factorcontour.h — 单因素等值线（m2/mapping-pages 任务 B）。
// PALEO_QGIS_PLAN §12：等值线是 GIS LineString 图层，不是画布临时线。
// 降级决议（§32/§33 评审钉死）：本机 Processing 注册表在 C++ 嵌入运行时
// 只含 paleo:*（native/gdal 等均不自动注册；gdal:contour 属 Python
// provider），因此 raster → 等值线走 GDAL C API GDALContourGenerateEx
// （gdal:contour 的同一底层引擎），产出真 LineString GPKG 图层，
// 禁止假图层。
// 层：QGIS 封装
namespace FactorContourService
{

// 从 rasterPath 的 band 1 生成等值线（间距 interval，等值线基准取 0）写入
// outputGpkg 的 "contours" 图层（字段 ID int64 / ELEV real，SRS 随栅格）。
// 输出文件已存在则覆盖（重生成幂等由调用方按 GPKG 路径 upsert 承载）。
bool generateContours(const QString &rasterPath, const QString &outputGpkg,
                      double interval, QString *error = nullptr,
                      const std::function<bool()> &cancelled = {},
                      const std::function<void(double)> &progress = {});

// 显式级别。不改间距接口的行为。级别必须非空且全部有限。
// cancelled 透传到 GDALContourGenerateEx 的进度回调（取消即中止提线）。
bool generateFixedContours( const QString &rasterPath, const QString &outputGpkg,
                            const QVector<double> &levels, QString *error = nullptr,
                            const std::function<bool()> &cancelled = {} );

// WS-C part2：structural_idw 面 → 上游 field_contours 提取（不走 GDAL 等值线，
// 不许静默降级）。输入 = Float64 栅格 + <stem>.structural.json 侧卡。
// levels 非空 → 显式级别（覆盖间距）；否则 interval<=0 → 自动（上游
// ContourExtractDialog 自适应规则），interval>0 → 用户间距。
// levelsUsed 回传实际级别（可空）。
// cancelled 在阶段边界（读栅格/加载侧卡/级别规划后）轮询；field_contours
// 提线内部暂无检查点，取消粒度为一整个阶段。
bool generateStructuralContours( const QString &rasterPath,
                                 const QString &structuralJsonPath,
                                 const QString &outputGpkg,
                                 const QVector<double> &levels,
                                 double interval,
                                 QVector<double> *levelsUsed = nullptr,
                                 QString *error = nullptr,
                                 const std::function<bool()> &cancelled = {},
                                 bool smooth = true );

// 普通分析场（无 .structural.json）按所选提线包走同一套 field contours。
// 轴对齐北向上栅格用像元中心建 field_only 表面；旋转栅格返回 false，
// 调用方不得把它记成制图规范提线。
bool generateAnalysisFieldContours( const QString &rasterPath,
                                    const QString &outputGpkg,
                                    const QVector<double> &levels,
                                    double interval,
                                    QVector<double> *levelsUsed = nullptr,
                                    QString *error = nullptr,
                                    const std::function<bool()> &cancelled = {},
                                    bool smooth = true );

void requestContours(const QString &rasterPath, const QString &outputGpkg,
                     double interval, PaleoTaskService *tasks,
                     QObject *receiver,
                     std::function<void(bool, const QString &)> ready,
                     std::shared_ptr<void> lifetime = {});
} // namespace FactorContourService
