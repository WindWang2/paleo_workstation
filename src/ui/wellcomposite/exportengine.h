// 层：视图
#pragma once

#include <QColor>
#include <QPagedPaintDevice>
#include <QString>

#include <QList>

#include "wellcompositetrack.h"
#include "domain/wellcompositemodel.h"

// ui/wellcomposite/exportengine — D4.5–D4.10/D4.12 导出引擎
//
// 离屏矢量/位图导出：
//   PDF（QPdfWriter 矢量，多页分页 + 页眉：井名/项目/日期/比例尺）
//   PNG（≥300dpi 等效分辨率位图）
//   SVG（QSvgGenerator 可用时；不可用返回错误原因）
//   打印预览/剪贴板复制（renderToImage + QClipboard）
// 渲染走与屏幕同一套道渲染器，但使用导出档渲染参数（D4.12：抗锯齿/线宽
// 分档——屏幕 1px 抗锯齿、导出 2px 加粗+全抗锯齿）。
//
// D4.4 自动图例：LegendGenerator 按当前井数据收集岩性/相/曲线/标志层符号。

namespace WellComposite
{

class WellCompositeCanvas;

class ExportEngine
{
public:
  enum class Format
  {
    Pdf,
    Png,
    Svg
  };

  struct Options
  {
    QString scaleRatio = QStringLiteral("1:500");
    double topDepth = 0.0;        // 导出深度区间（整井 = min~max）
    double bottomDepth = 3000.0;
    bool includeHeader = true;    // 页眉（井名/项目/日期/比例尺）
    bool includeLegend = true;    // 图例区
    int dpi = 300;
    QString wellName;
    QString projectName;
    // D4.8 分页策略：每页纵向米数（0 = 按 dpi 与页面高度自适应）
    double metersPerPage = 0.0;
  };

  // 导出画布当前可见道（isPrintIncluded()==false 的道跳过）到 path。
  // 成功返回空串；失败返回错误原因。
  static QString exportCanvas(const WellCompositeCanvas &canvas, const ComprehensiveWellData &data,
                              Format format, const QString &path, const Options &opt);

  // D3（wave/deepen-perf）原生打印接线：分页绘制到任意 QPagedPaintDevice
  //（QPdfWriter 与 QPrinter 共用——打印与 PDF 导出同一渲染管线；标题/布局
  // 由调用方在 device 上预设）。成功返回空串。
  static QString exportToPagedDevice(const WellCompositeCanvas &canvas,
                                     const ComprehensiveWellData &data,
                                     QPagedPaintDevice &device, const Options &opt);

  // D3：系统打印机探测（offscreen/无打印服务 → false → 打印入口降级 PDF）
  static bool nativePrintAvailable();

  // D4.6 PNG ≥dpi 等效分辨率渲染（返回空图 = 失败）
  static QImage renderToImage(const WellCompositeCanvas &canvas, const ComprehensiveWellData &data,
                              const Options &opt, QString *errorOut = nullptr);

  // D4.9 打印预览图（整井快览，固定高度）
  static QImage renderPreview(const WellCompositeCanvas &canvas, const ComprehensiveWellData &data,
                              int heightPx);

  // D4.12 双渲染参数：应用导出档画笔（抗锯齿全开/线宽加粗）
  static void applyExportRenderHints(QPainter &painter);

  // 页眉内容行（井名/项目/日期/比例尺）——导出与测试共用
  static QStringList headerLines(const Options &opt);
};

// ----------------------------------------------------------------------------
// D4.4 自动图例生成器：当前井用到的岩性/相/曲线符号排成图例区
// ----------------------------------------------------------------------------
class LegendGenerator
{
public:
  struct LegendEntry
  {
    QString symbol;   // 岩性名/相名/曲线名/标志层名
    QString detail;   // 花纹/纹理键、量程、单位等
    QColor swatch;    // 色块（曲线为线色）
    bool isLine = false; // 曲线画线样例，否则色块
  };

  // 按井数据收集图例项（岩性花纹 / 相纹理 / 曲线量程 / 标志层）
  static QList<LegendEntry> collect(const ComprehensiveWellData &data,
                                    const QList<std::shared_ptr<WellTrack>> &tracks);

  // 绘制图例区到 painter（自动分两列）；返回占用高度
  static qreal paint(QPainter &painter, const QRectF &rect, const QList<LegendEntry> &entries);

  // 图例文本行（TSV；测试与文档用）
  static QStringList textLines(const QList<LegendEntry> &entries);
};

} // namespace WellComposite
