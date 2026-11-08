// 层：QGIS 封装
#pragma once
#include <QList>
#include <QString>
#include <QStringList>

#include "layoutexport.h"   // PaleoLayoutExport::Format
#include "mapbooklayout.h"  // PaleoMapBookLayout::RenderOutcome

class QgsMapLayer;
class QgsProject;

// qgis/mapbookrender — 地图册渲染 facade（P5-W6 边界收敛）。
//
// 旧况：workflow/mapbookqueue 直接 include qgsprintlayout/qgsrectangle 并
// build → render → delete 版面对象，QGIS 实体的建/渲/毁细节漏进功能层。
// 本 facade 把「建版、渲染、销毁」三步关进封装层，workflow 只描述要什么
// （TileJob/IndexJob 纯值 + 图层/工程指针透传——Request.layers 的既有口
// 径不变），拿到 RenderOutcome 记账。版面构造失败、渲染失败、产物校验失
// 败都折算成 outcome.ok=false + 人读 error，调用方不再接触 QgsPrintLayout。
//
// 收敛范围刻意最小：只承接地图册调用链（mapbookqueue），不扩到
// mapexport/horizonbatchexport/layouttemplatelibrary 全族。
namespace PaleoMapBookRender
{

// 逐格版面渲染请求（字段语义同 PaleoMapBookLayout::TileSpec；范围为纯值，
// workflow 不再构造 QgsRectangle）。
struct TileJob
{
    QString title;
    QString footer;
    double xMin = 0.0;
    double yMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
    QList<QgsMapLayer *> layers;
    bool landscape = true;
    bool legend = true;
    bool scaleBar = true;
    bool northArrow = true;
    bool crsCaption = true;
};

// 目录索引页渲染请求（分页在封装层内完成，见 mapbooklayout 的分页口径）。
struct IndexJob
{
    QString title;
    QStringList entries;
    bool landscape = false;
};

// 建版面 → 渲染 → 逐文件校验 → 销毁，一步完成；版面对象不出封装层。
PaleoMapBookLayout::RenderOutcome renderTile( QgsProject *project, const TileJob &job,
                                              const QString &outPath,
                                              PaleoLayoutExport::Format format, double dpi );
PaleoMapBookLayout::RenderOutcome renderIndex( QgsProject *project, const IndexJob &job,
                                               const QString &outPath,
                                               PaleoLayoutExport::Format format, double dpi );

// ---- 图层/工程句柄最小包装（workflow 去 qgs*.h include 后仍能取 id/找层）----

QString layerStableId( const QgsMapLayer *layer );

QgsMapLayer *projectLayerById( QgsProject *project, const QString &id );

// P5-W3：图层次序快照（纯值）——id/显示名/类型/CRS authid/受管身份属性/
// 数据源 URI。无效 CRS 与缺失属性如实留空，不填默认值。
struct LayerSummary
{
    QString id;
    QString name;
    QString type;     //!< vector/raster/plugin/annotation/mesh/...
    QString crs;      //!< authid（空 = 无效/未知）
    QString paleoId;  //!< paleoLayerId 自定义属性
    QString assetId;  //!< paleoAssetId 自定义属性
    QString versionId; //!< paleoVersionId 自定义属性
    QString source;   //!< 数据源 URI
};
LayerSummary summarizeLayer( const QgsMapLayer *layer );

// 工程有效 CRS 的 authid（空 = 无效/未知）。
QString projectCrsAuthid( const QgsProject *project );

} // namespace PaleoMapBookRender
