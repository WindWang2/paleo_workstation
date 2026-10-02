// 层：功能
#pragma once
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

// workflow/mapbook — 地图册（map book）序列与模板变量的纯编排核（功能层）。
//
// 职责边界：只回答「AOI 怎么切成格序列」「格序列的名字/中心/范围是什么」
// 「模板串里的变量怎么替换」，不碰 QGIS、不碰 QtWidgets、不建任何窗口或
// 对话框；版面与落盘在 qgis/mapbooklayout（QGIS 封装层），批量调度在
// workflow/mapbookqueue。
//
// 坐标一律是工程坐标（米、未投影），与预览画布/厚度栅格的口径一致；本模块
// 不做任何 CRS 换算——换算是 qgis 层的事。
//
// 模板变量语法：`%{name}`。替换是**严格**的：引用了未定义的变量 → 返回空串
// 并在 *error 里写明（绝不静默留空，否则版面会印出半截标题而无人察觉）；
// `%{` 没有配对的 `}` 同样报错。这一条是 Oracle 4 的可断言契约。
namespace PaleoMapBook
{

// AOI 矩形（工程坐标，米）。xMax>xMin 且 yMax>yMin 才算合法。
struct Area
{
    double xMin = 0.0;
    double yMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
    QString name; //!< 可选：给定多边形集时的分区名

    bool valid() const { return xMax > xMin && yMax > yMin; }
    double width() const { return xMax - xMin; }
    double height() const { return yMax - yMin; }
    double centerX() const { return 0.5 * ( xMin + xMax ); }
    double centerY() const { return 0.5 * ( yMin + yMax ); }
    // 人读范围串：`x0, y0 → x1, y1`（两位小数，版面页脚/索引页直接用）。
    QString extentText() const;
};

// 格序列的遍历次序：行主序（先走完一行再换行）/ 列主序。
enum class Order
{
    RowMajor,
    ColumnMajor
};

struct GridRequest
{
    Area area;
    int cols = 0;
    int rows = 0;
    Order order = Order::RowMajor;
    // 命名模板（走同一套 %{...} 变量）。缺省 `tile_%{tile_row}_%{tile_col}`，
    // 行列为 1-based（人对格号的习惯）。变量名必须与 variableNames() 一致——
    // 引用表外名字时 buildGrid 会整批拒绝，不会印出半截格名。
    QString namePattern = QStringLiteral( "tile_%{tile_row}_%{tile_col}" );
};

struct Tile
{
    int index = 0;   //!< 序列序号，0-based，按 GridRequest::order
    int row = 0;     //!< 1-based 行号（网格）；给定多边形集时恒为 1
    int col = 0;     //!< 1-based 列号（网格）；给定多边形集时 = index+1
    QString name;    //!< 由 namePattern 替换而来；落盘文件名即它
    Area extent;     //!< 本格范围
};

// 规则网：把 AOI 均分成 cols×rows 格。非法输入（区域不合法 / cols 或 rows
// < 1 / 命名模板引用未定义变量）→ 空序列 + *error。
QVector<Tile> buildGrid( const GridRequest &request, QString *error = nullptr );

// 给定多边形（矩形）集：一格一个 Area，次序即输入次序；Area::name 为空时
// 退回 `area_%{tile_index}`（index 0-based）。非法 Area 逐个报错并整体失败
// （不静默丢格）。
QVector<Tile> tilesFromAreas( const QVector<Area> &areas, QString *error = nullptr );

// 模板变量表（单一真源）：tile_index/tile_row/tile_col/tile_name/tile_label/
// center_x/center_y/x_min/y_min/x_max/y_max/width/height/extent/date/book/
// horizon/crs。凡是要往版面里印的东西都从这里取，别在别处手拼。
QStringList variableNames();

struct TileContext
{
    Tile tile;
    QString book;     //!< 图册名
    QString horizon;  //!< 层位（可空）
    QString crs;      //!< CRS 说明（可空 → 缺省「工程坐标 · 米 · 未投影」）
    QString date;     //!< ISO 日期；空 → 取当天
};

QVariantMap tileVariables( const TileContext &context );

// 严格替换：text 里的每个 `%{name}` 必须在 vars 里有定义。
// 成功返回替换后的串；任一变量未定义 / `%{` 未闭合 → 返回空串 + *error。
QString applyVariables( const QString &text, const QVariantMap &vars, QString *error = nullptr );

// text 引用的变量名（按出现次序去重）。批量开始前用它做一次「缺变量」预检，
// 避免跑到第 N 版才发现标题模板印不出来。
QStringList referencedVariables( const QString &text, QString *error = nullptr );

} // namespace PaleoMapBook
