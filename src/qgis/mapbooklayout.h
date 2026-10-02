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

struct IndexSpec
{
    QString title;            //!< 索引页标题
    QStringList entries;      //!< 每行一条（格号 + 范围 + 文件名）
    bool landscape = false;   //!< 目录页默认竖版
};

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
    QString error;    //!< 人读，成功时为空
    QString path;     //!< 实际落盘文件（导出核心可能补扩展名）
    qint64 bytes = 0; //!< 文件大小（断言非空）
    int width = 0;    //!< 像素宽（PNG；PDF 为 0）
    int height = 0;   //!< 像素高（PNG；PDF 为 0）
};

// 导出 + 回读校验。format/dpi 语义与 PaleoLayoutExport::exportLayout 一致。
RenderOutcome renderLayout( QgsPrintLayout *layout, const QString &outPath,
                            PaleoLayoutExport::Format format, double dpi );

} // namespace PaleoMapBookLayout
