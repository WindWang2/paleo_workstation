// 层：QGIS 封装
#pragma once

#include <QHash>
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QPainter>
#include <QPointer>
#include <QRect>
#include <QStack>
#include <QString>
#include <QWidget>

#include <qgscoordinatereferencesystem.h>
#include <qgspointxy.h>
#include <qgsrectangle.h>

#include <functional>

class QgsMapCanvas;
class QgsMapLayer;
class QgsMapTool;
class QgsProject;
class QTimer;

// qgis/previewmapcanvas — 数据预览专用画布封装（P2 D1.1）。
//
// 所有可地图化的预览（层位栅格/GeoJSON 相图/配准图片/井位图/测区全景）
// 统一经本类上图，替代此前每个分支手搓 QgsMapCanvas 的散装写法：
//   · 私有层容器——层序/可见性/透明度/混合模式由本类记账，层不注册
//     QgsProject（既有约定：预览不污染主图图层树/实例表）；
//   · 工程 CRS 固定——缺省钉 DataCatalog::localGridCrsWkt() 的 datum-free
//     局部工程网格（米），绝不允许 QGIS 隐式 EPSG:4326；栅格自带 CRS 与
//     画布不一致时按 overrideCrs 让位（层 CRS 优先，分支自行决定语义）；
//   · 渲染状态——renderStarting/mapCanvasRefreshed 接成 renderStarted/
//     renderCompleted(ms, layers, elements)，渲染中可查询（D1.6）；
//   · 销毁/切换安全——析构与换层前 stopRendering() + unsetMapTool()，
//     预览关闭时异步渲染任务全部取消，无悬挂 job（D1.9）；
//   · 视图历史栈——back/forward 双栈去重（D3.3）；
//   · 渐进渲染——renderSnapshot() 低分辨率整图先行 + showPreviewOverlay()
//     缓存图立即上屏、renderCompleted 后让位真渲（D6.1/D6.2）。
class PreviewMapCanvas : public QWidget
{
    Q_OBJECT
  public:
    explicit PreviewMapCanvas( QWidget *parent = nullptr );
    ~PreviewMapCanvas() override;

    // 内嵌 QgsMapCanvas（objectName 由调用方命名以保持既有测试兼容面）。
    QgsMapCanvas *canvas() const { return m_canvas; }

    // ---- 私有层容器（index 0 = 顶 = 渲染最上层；层所有权归调用方父子树）----
    void setLayers( const QList<QgsMapLayer *> &layers );
    void addLayer( QgsMapLayer *layer );                 // 加到顶部
    void insertLayer( int index, QgsMapLayer *layer );
    void removeLayer( QgsMapLayer *layer );
    void moveLayer( int from, int to );
    int layerCount() const;
    QList<QgsMapLayer *> layers() const;                 // 顶到底（含隐藏层）
    int indexOfLayer( const QgsMapLayer *layer ) const;
    QgsMapLayer *layerAt( int index ) const;
    // 可见性/透明度/混合是本类的记账状态（QgsMapLayer 无图层树可见位）；
    // 不可见层不进 setLayers。
    void setLayerVisible( QgsMapLayer *layer, bool visible );
    bool isLayerVisible( const QgsMapLayer *layer ) const;
    void setLayerOpacity( QgsMapLayer *layer, double opacity );      // 0..1
    double layerOpacity( const QgsMapLayer *layer ) const;
    void setLayerBlendMode( QgsMapLayer *layer, QPainter::CompositionMode mode );
    QPainter::CompositionMode layerBlendMode( const QgsMapLayer *layer ) const;

    // ---- CRS ----
    // 缺省 datum-free 局部网格；层自带可信 CRS 的预览（如 GeoJSON 经纬度）
    // 由分支调 setOverrideCrs 让画布跟随层（旧 facies 画布语义）。
    void setOverrideCrs( const QgsCoordinateReferenceSystem &crs );
    QgsCoordinateReferenceSystem crs() const;

    // ---- 范围与视图历史 ----
    QgsRectangle fullExtent() const;                     // 可见层联合范围
    QgsRectangle currentExtent() const;
    void zoomToFullExtent();
    void zoomToLayer( const QgsMapLayer *layer );
    void zoomToRect( const QgsRectangle &rect );
    // 精确复位（不另加边距）——书签跳转用（D3.7：存的就是当时的范围）。
    void setViewExtent( const QgsRectangle &rect );
    bool canZoomBack() const;
    void zoomBack();
    bool canZoomForward() const;
    void zoomForward();
    void clearHistory();

    // ---- 渲染状态（D1.6）----
    bool isRendering() const { return m_rendering; }
    qint64 lastRenderDurationMs() const { return m_lastRenderMs; }
    qint64 lastRenderElementCount() const { return m_lastRenderElements; }
    void cancelRendering();

    // ---- 渐进渲染（D6.1/D6.2）----
    // 以当前图层/范围低分辨率同步渲一张整图（QgsMapRendererCustomPainterJob），
    // 供「先出低清、后台精渲替换」的第一段。maxWidthPx<=0 时用画布当前宽。
    QImage renderSnapshot( int maxWidthPx = 320 ) const;
    // 立即显示一张（缓存的或低清的）图盖在画布上；renderCompleted 自动隐藏。
    void showPreviewOverlay( const QImage &image );
    void hidePreviewOverlay();
    bool overlayVisible() const;

    // ---- 坐标换算/比例（状态条用）----
    QgsPointXY toMapCoordinates( const QPoint &pixel ) const;
    double mapUnitsPerPixel() const;
    double scale() const;

    // ---- 工程图层桥接（测区全景：项目图层树 → 画布，私有层容器退位）----
    // 绑定后本类不再向画布 setLayers（桥接管），其余能力不变。
    void attachProjectLayers( QgsProject *project );
    bool isProjectBound() const { return m_projectBound; }

    // 元素量估算（状态条「图元数」）：矢量要素数 + 栅格可见像元数。
    static qint64 estimateElements( const QList<QgsMapLayer *> &layers );

  signals:
    void layersChanged();
    void extentChanged( const QgsRectangle &extent );
    void renderStarted();
    void renderCompleted( qint64 durationMs, int layerCount, qint64 elementCount );
    void scaleChanged( double scale );
    void crsChanged();
    void mapPositionTracked( const QgsPointXY &point );  // 鼠标移动（状态条坐标）

  protected:
    // D1.8 键盘：+/- 缩放（以视口中心）、0 全图复位、方向键平移 20%。
    void keyPressEvent( QKeyEvent *event ) override;
    // 鼠标移动（节流）→ mapPositionTracked；覆盖层跟随画布尺寸。
    bool eventFilter( QObject *watched, QEvent *event ) override;

  private:
    void pushHistory( const QgsRectangle &extent );
    void syncCanvasLayers();
    void setExtentInternal( const QgsRectangle &rect );

    QgsMapCanvas *m_canvas = nullptr;
    class QLabel *m_overlay = nullptr;   // 低清/缓存快照覆盖层
    QList<QgsMapLayer *> m_layers;       // 顶到底（含隐藏）
    QHash<const QgsMapLayer *, bool> m_visible;
    QHash<const QgsMapLayer *, double> m_opacity;
    QHash<const QgsMapLayer *, QPainter::CompositionMode> m_blend;

    QgsCoordinateReferenceSystem m_overrideCrs;
    bool m_projectBound = false;

    // 视图历史（D3.3）：程序式缩放与交互平移都入栈；去重相邻同范围。
    QStack<QgsRectangle> m_backStack;
    QStack<QgsRectangle> m_forwardStack;
    bool m_suppressHistory = false;

    // 渲染状态
    bool m_rendering = false;
    qint64 m_lastRenderMs = 0;
    qint64 m_lastRenderElements = 0;
    QElapsedTimer m_renderTimer;
    // 悬停节流（D6.5 ≤30Hz）：mapPositionTracked 至少间隔 33ms。
    QElapsedTimer m_trackTimer;
    qint64 m_lastTrackMs = 0;

    friend class PreviewMapCanvasSelftest; // 测试直查内部栈
};
