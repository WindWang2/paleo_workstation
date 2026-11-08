// 层：QGIS 封装
#pragma once
#include <QString>
#include <QStringList>
#include <qgsrectangle.h>

#include "layoutexport.h" // PaleoLayoutExport::Format/exportLayout（同层导出核心，复用不自写）

class QgsMapLayer;
class QgsPrintLayout;
class QgsProject;

// qgis/mapbooklayout — 地图册三类版面的构建与落盘（QGIS 封装层）。
//
// 复用边界（禁区：不改既有导出契约形状）：导出一律走
// PaleoLayoutExport::exportLayout（QgsLayoutExporter 封装），本模块只在它
// 之上加「版面怎么摆」和「产物落盘后回读校验」两件事。
//
// 三类版面：
//   1. 逐格版面 buildTileLayout —— 固定模板位：标题 / 地图 / 图例 / 比例尺 /
//      指北针 / 页脚（CRS + 格号坐标），变量替换已在功能层做完，这里只印串。
//   2. 蒙太奇 buildMontageLayout —— 一版面多图项同源联动：平面图（真地图项）
//      + 剖面快照（图片项）+ 连井小图（图片项），各带小标题。
//   3. 目录索引页 buildIndexLayout —— 报告装配的最小面（多版目录一页）。
//
// renderLayout 是唯一落盘出口：导出成功后再回读文件（非空 + 格式文件头 +
// 像素尺寸），把「产物真的能打开」变成可断言事实，而不是看截图。
namespace PaleoMapBookLayout
{

struct TileSpec
{
    QString title;               //!< 标题（变量已替换）
    QString footer;              //!< 页脚（格号/中心坐标/CRS，变量已替换）
    QgsRectangle extent;         //!< 本格范围
    QList<QgsMapLayer *> layers; //!< 地图项图层（可空 → 空白地图项）
    bool landscape = true;       //!< A4 横版 / 竖版
    bool legend = true;
    bool scaleBar = true;
    bool northArrow = true;
    bool crsCaption = true;
};

struct MontageSpec
{
    QString title;
    QString mapCaption;
    QString sectionCaption;
    QString wellCaption;
    QList<QgsMapLayer *> mapLayers;
    QgsRectangle mapExtent;
    QString sectionImage; //!< 剖面快照 PNG 路径（渲染管线产物）
    QString wellImage;    //!< 连井小图 PNG 路径
    bool landscape = true;
};

// 目录索引页规格（P5-W5 分页口径）：条目超出单页容量时按实际纸面/字号/
// 行高/长名占行自动分页（多页版面，一页一条目块），不再截断到 60 行。
// 页容量 = (页高 − 标题区 − 下边距) / 行距，条目占行按可用宽度与字体
// 度量估算换行；渲染后多页 PNG/PDF 的全部兄弟文件经 RenderOutcome::files
// 回传并逐文件校验。
struct IndexSpec
{
    QString title;            //!< 索引页标题（多页时各页附「第 i/N 页」）
    QStringList entries;      //!< 每行一条（格号 + 范围 + 文件名），次序保留
    bool landscape = false;   //!< 目录页默认竖版
};

// 目录分页的纯计算面（buildIndexLayout 同一事实源；测试断言页数用）。
// 返回给定条目在指定版式下占的页数（≥1——空目录也出一页标题页）。
int indexPageCount( const QStringList &entries, bool landscape );

// 返回堆对象，调用方负责 delete（与 PaleoLayoutExport::buildHorizonMapLayout
// 同纪律）；失败 → nullptr + *error。
QgsPrintLayout *buildTileLayout( QgsProject *project, const TileSpec &spec,
                                 QString *error = nullptr );
QgsPrintLayout *buildMontageLayout( QgsProject *project, const MontageSpec &spec,
                                    QString *error = nullptr );
QgsPrintLayout *buildIndexLayout( QgsProject *project, const IndexSpec &spec,
                                  QString *error = nullptr );

struct RenderOutcome
{
    bool ok = false;
    QString error;      //!< 人读，成功时为空
    QString stage;      //!< 失败阶段提示：layout=建版失败；export=渲染/校验失败；成功为空
    QString path;       //!< 主产物（多页导出的首页；导出核心可能补扩展名）
    QStringList files;  //!< P5-W5：全部落盘文件（含 _2.. 兄弟页），主产物在前
    qint64 bytes = 0;   //!< 主产物大小（断言非空）
    qint64 totalBytes = 0; //!< 全部文件合计（与 files 一一对应求和）
    int width = 0;      //!< 主产物像素宽（PNG；PDF 为 0）
    int height = 0;     //!< 主产物像素高（PNG；PDF 为 0）
};

// 导出 + 回读校验（P5-W5 起逐文件校验：多页导出的每个兄弟文件都要过
// validateExportedFile，首页存在不等于全部成功）。format/dpi 语义与
// PaleoLayoutExport::exportLayout 一致。
RenderOutcome renderLayout( QgsPrintLayout *layout, const QString &outPath,
                            PaleoLayoutExport::Format format, double dpi );

// 产物回读校验（renderLayout 的唯一校验口；独立暴露供坏文件注入测试）。
// PNG：8 字节签名 + 可解码 + 像素尺寸 ≈ expected（±2px 取整容差）；
// PDF：%PDF 头 + 末尾 1KiB 内 %%EOF（截断即失败）；
// SVG：头 4KiB 内含 <svg 根元素。
// expectedWidthPx/expectedHeightPx ≤ 0 → 跳过像素尺寸校验（PDF/SVG 无像素面）。
bool validateExportedFile( const QString &path, PaleoLayoutExport::Format format,
                           double expectedWidthPx = -1.0, double expectedHeightPx = -1.0,
                           QString *error = nullptr );

} // namespace PaleoMapBookLayout
