// 层：视图
#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <qgsrectangle.h>

#include <functional>

class QWidget;
class DataCatalog;
struct CatalogVersion;

// ui/datapreview/previewmapstates — 预览地图页的状态页与会话记忆
//（P2 D1.7/D2.7/D2.10/D2.11/D2.12/D3.7/D4.7）。纯视图辅助，无画布依赖。

namespace PreviewMapStates
{

// D1.7 数据源损坏/CRS 异常统一原因页：标题 + 详情 + 可选重试按钮。
// 返回的页 objectName="previewMapErrorPage"，正文 label objectName="stateText"
//（与既有失败态同一断言面）；retryText 为空时不给按钮。
QWidget *buildErrorPage( const QString &title, const QString &detail, QWidget *parent,
                         const QString &retryText = QString(),
                         const std::function<void()> &onRetry = nullptr );

// D2.12 未知类型资产统一「不支持预览」态：类型 + 可支持的类型列表。
// 返回页 objectName="previewUnsupportedPage"。
QWidget *buildUnsupportedPage( const QString &typeName, QWidget *parent );

// D2.11 大图降级提示条（hint 非空才有意义；bigRasterHint 的出口文案）。
QWidget *buildBigRasterHintBar( const QString &hint, QWidget *parent );

// D2.7 world file 探测：PNG/JPG 身旁的配准边车（.wld/.pgw/.jgw/.tfw/.hpw
// 及同名 .wld）。返回存在的边车绝对路径；无 → 空串。
QString detectWorldFile( const QString &imagePath );

// D2.7 配准对落位：托管副本身旁没有 world file、但源文件身旁有 → 把
// 「图片+边车」成对复制进 QTemporaryDir（owner 随 parent 生命周期），
// 返回 {图片, 边车} 的新路径；不需要搬对时原样返回输入。
QPair<QString, QString> stageGeorefPairIfNeeded( const QString &imagePath,
                                                 const QString &sourceImagePath,
                                                 QObject *parent );

// D2.10 同目录组图：与 anchorPath 同目录、可地图化叠加的资产
//（geojson / 带配准 image_reference / horizon——后两者按文件可上图判）。
// resolveAbs 把版本解析成绝对路径（调用方持 PreviewDocService 门面）。
// 返回 (assetId, displayName) 列表，不含 anchorPath 自身。
QVector<QPair<QString, QString>> siblingMappableAssets(
    DataCatalog *catalog, const QString &anchorPath,
    const std::function<QString( const CatalogVersion & )> &resolveAbs );

// 可支持的预览类型清单（D2.12 文案用）。
QStringList supportedPreviewTypes();

} // namespace PreviewMapStates

// ---------------------------------------------------------------- session --
// 会话级预览状态记忆（D3.7 书签 / D4.7 TOC 状态随资产记忆）。
// 进程内静态：标签重建后状态仍在；不落盘（写 QSettings 只发生在用户显式
// 动作——主题开关同口径）。
class PreviewStateMemory
{
  public:
    struct Bookmark
    {
      QString name;
      QgsRectangle extent;
    };
    struct TocLayerState
    {
      bool visible = true;
      double opacity = 1.0;
    };

    static QVector<Bookmark> bookmarks( const QString &assetKey );
    static void addBookmark( const QString &assetKey, const Bookmark &bm );
    static bool removeBookmark( const QString &assetKey, const QString &name );
    // 按（层名 → 状态）记忆 TOC；层序单独记（顶到底的名字序）。
    static QHash<QString, TocLayerState> tocStates( const QString &assetKey );
    static QStringList tocOrder( const QString &assetKey );
    static void saveToc( const QString &assetKey,
                         const QStringList &orderTopToBottom,
                         const QHash<QString, TocLayerState> &states );
    // 单层移除（TOC removeLayer 用——增量加层期间不能整表重写）。
    static void removeTocLayer( const QString &assetKey, const QString &layerName );
    static void clearAsset( const QString &assetKey );
    // 测试清场。
    static void clearAll();
};
