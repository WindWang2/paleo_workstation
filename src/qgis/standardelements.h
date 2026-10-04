// 层：QGIS 封装
#pragma once
#include <QList>
#include <QString>

class QgsLayoutItem;
class QgsLayoutItemLabel;
class QgsLayoutItemLegend;
class QgsLayoutItemMap;
class QgsLayoutItemScaleBar;
class QgsMapLayer;
class QgsPrintLayout;
class QgsProject;
class QgsRectangle;
class QgsTextFormat;

// qgis/standardelements — Paleo 标准图件元素工厂 + 三类内置内容模板骨架
//（goal 方向 25 M1/M2/M3 的 QGIS 封装层事实源）。
//
// 每个工厂负责同一件事：创建元素 → 套 Paleo 图件默认值 → addLayoutItem →
// 按 DESIGN.md「画布内装饰」位约定定位（指北针左上、比例尺左下、图例右上、
// 图件标题左上；标题 15pt / 副题 9pt / 署名 8pt，Noto Sans CJK SC）。
// 定位基准是**地图项的 scene 矩形**（装饰随地图走）或**页面**（标题块/署名），
// 这样同一工厂既能给整幅模板排版，也能给设计器「添加标准元素」菜单用——
// 用户加完再拖。所有权：全部项经 addLayoutItem 交给 layout（QGraphicsScene
// 父子），工厂只回裸指针。
//
// 三类内容模板（FigureKind）只决定**版面骨架差异**：
//   WellPosition   井位图——地图通栏（无右图例列），小图例浮于地图右上；
//   SingleFactor   单因素图——右侧图例列（色带说明要占位），地图左列；
//   Facies         沉积相图——右侧宽图例列 + 右下插图（全图位置指示，overview
//                  框随主图范围联动）。
// 图层与范围由调用方给（模板不做数据决策）；空图层也合法（骨架先立起来，
// 地图项由后续绑定/快照逻辑填内容）。
namespace PaleoStandardElements
{

// ---- 图件种类（词表单一事实）---------------------------------------------
enum class FigureKind
{
  WellPosition,  //!< 井位图
  SingleFactor,  //!< 单因素图
  Facies,        //!< 沉积相图
};

QString figureKindKey( FigureKind kind );                    //!< "well_position" / "single_factor" / "facies"
bool figureKindFromKey( const QString &key, FigureKind *out ); //!< false = 未知 key
QString figureKindTitle( FigureKind kind );                  //!< 井位图 / 单因素图 / 沉积相图（tr 源串中文）

// ---- 页面规格 --------------------------------------------------------------
struct PageSetup
{
  QString sizeName = QStringLiteral( "A4" ); //!< QGIS 标准纸名（setPageSize 直收）
  bool landscape = true;
};
//! 应用到 layout 第 0 页（无页时先补一页）。false = layout 空/纸名不被 QGIS 认识。
bool applyPageSetup( QgsPrintLayout *layout, const PageSetup &setup );

// ---- 图件文本/装饰共用助手 --------------------------------------------------

//! DESIGN.md 字阶的图件文本格式（Noto Sans CJK SC，pt 单位）。
QgsTextFormat figureTextFormat( double sizePt );

//! QGIS 自带指北针 SVG 的第一个可用路径（svgPaths 搜索）；无则空串（退化「N」标注）。
QString northArrowSvgPath();

// ---- 标准图件元素工厂 ------------------------------------------------------

//! 主地图项（id="map"）。extent 为空时不动范围（保持 QGIS 缺省整图）。
QgsLayoutItemMap *addMainMap( QgsPrintLayout *layout, const QList<QgsMapLayer *> &layers,
                              const QgsRectangle &extent );

//! 插图（全图位置指示）：右下小图，overview 框钉主图范围（M3 联动的落点）。
//! 图层沿用主图当前图层；范围 = 主图图层的全幅（主图范围只是里面的一个框）。
QgsLayoutItemMap *addInsetMap( QgsPrintLayout *layout, QgsLayoutItemMap *mainMap );

//! 比例尺（数字+条式）：Single Box + 段落数字，米制，钉主图（比例随主图联动）。
QgsLayoutItemScaleBar *addScaleBar( QgsPrintLayout *layout, QgsLayoutItemMap *map );

//! 图例（随图层树自动更新）：AllProjectLayers 同步 + 按地图过滤；要手动删减
//! 条目时在属性面板切 Manual（QGIS 原生语义，不在这里复制）。
QgsLayoutItemLegend *addLegend( QgsPrintLayout *layout, QgsLayoutItemMap *map );

//! 指北针（左上）：QGIS SVG 箭头，随地图旋转；无 SVG 资源时退化「N」标注。
QgsLayoutItem *addNorthArrow( QgsPrintLayout *layout, QgsLayoutItemMap *map );

//! 坐标网格（图上经纬/坐标网）：按范围算整刻度间隔，带注记与外框。
//! 空范围（无 CRS/无范围）时退化为 1000m 缺省间隔，仍可出图。
void addCoordinateGrid( QgsLayoutItemMap *map );

struct TitleBlock
{
  QgsLayoutItemLabel *title = nullptr;     //!< 图名（15pt，页面左上）
  QgsLayoutItemLabel *subtitle = nullptr;  //!< 副题（9pt，图名下）
  QgsLayoutItemLabel *signature = nullptr; //!< 署名（8pt，页面右下）
};

//! 图名+副题+署名文本块（页面级定位，不随地图）。
TitleBlock addTitleBlock( QgsPrintLayout *layout, const QString &title, const QString &subtitle );

//! 标准元素全家桶（对既有地图项补装饰 + 标题块）。返回补齐的元素计数。
//! 给「添加全套标准元素」菜单与整幅模板共用的一条路径。
int addStandardSet( QgsPrintLayout *layout, QgsLayoutItemMap *map,
                    const QString &title, const QString &subtitle );

// ---- 三类内容模板 ----------------------------------------------------------

//! 按 kind + 页面规格构建整幅模板版面（新 QgsPrintLayout，调用方负责 ownership：
//! 未入 QgsLayoutManager 时自己 delete；入了则 manager 拥有）。
//! 图层/范围为空时地图项留空骨架（后续绑定）。
QgsPrintLayout *buildFigureLayout( FigureKind kind, const PageSetup &setup, QgsProject *project,
                                   const QList<QgsMapLayer *> &layers, const QgsRectangle &extent,
                                   QString *error = nullptr );

//! 把内容模板套进既有版面：清内容项（页面集合保留）→ 按规格改纸 → 重建骨架。
//! 设计器「套用内置模板」与批量导出换骨架共用（不动 layout 名字/所有权）。
bool populateFigureLayout( QgsPrintLayout *layout, FigureKind kind, const PageSetup &setup,
                           const QList<QgsMapLayer *> &layers, const QgsRectangle &extent,
                           QString *error = nullptr );

} // namespace PaleoStandardElements
