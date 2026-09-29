// 层：视图
#pragma once

#include <QWidget>

#include "../../qgis/previewmapcanvas.h"
#include "../../qgis/previewmaptools.h"

#include <qgspointxy.h>

#include <functional>

class QLabel;
class QMenu;
class QStackedWidget;
class QTabWidget;
class QToolBar;
class QAction;
class QActionGroup;
class QgsMapLayer;
class QgsRubberBand;
class PaleoDecorationManager;
class PreviewTocPanel;
class PreviewIdentifyPanel;
class PreviewProfilePanel;
class PreviewIdentifyCore;

// ui/datapreview/previewmappage — 预览地图组合页（P2 框架装配）。
// 所有可地图化资产的预览正文：上下文工具条（D1.3，按内容类型显隐）+
// QGIS 画布（D1.1 PreviewMapCanvas）+ 鹰眼（D3.9）+ 右键菜单（D1.4）+
// 状态条（D1.6 渲染状态 / D3.6 坐标与比例尺读数 / D3.4-3.5 量测读数）+
// 迷你 TOC 侧栏（D4.x）+ 识别结果侧栏（D7.x）+ 剖面侧栏（D5.x）+
// 统一错误态（D1.7）+ 键盘（D1.8，在 PreviewMapCanvas 内）+ 书签（D3.7）+
// 复制坐标/截图（D3.8）。分支只管：建层 → addMapLayer → 按内容开工具。
class PreviewMapPage : public QWidget
{
    Q_OBJECT
  public:
    explicit PreviewMapPage( QWidget *parent = nullptr );
    ~PreviewMapPage() override;

    // ---- 核心部件 ----
    PreviewMapCanvas *mapCanvas() const { return m_canvas; }
    PreviewMapToolManager *toolManager() const { return m_tools; }
    PaleoDecorationManager *decorations() const { return m_decor; }
    PreviewTocPanel *tocPanel() const { return m_toc; }
    PreviewIdentifyPanel *identifyPanel() const { return m_identify; }
    PreviewProfilePanel *profilePanel() const { return m_profile; }

    // ---- 层管理（画布 + TOC 双写；层所有权仍归分支父子树）----
    void addMapLayer( QgsMapLayer *layer, const QString &name, const QString &sourcePath );
    void removeMapLayer( QgsMapLayer *layer );
    void clearMapLayers();
    int mapLayerCount() const;

    // ---- D1.3 工具显隐（内容类型驱动；id = PreviewMapToolManager::k*）----
    void setToolVisible( const QString &toolId, bool visible );
    bool isToolVisible( const QString &toolId ) const;

    // ---- D1.7 错误态 ----
    void setError( const QString &title, const QString &detail = QString() );
    void clearError();
    bool errorActive() const;

    // ---- D5.1 剖面工具使能（栅格内容开；矢量关）----
    void setProfileEnabled( bool on );

    // ---- D5.x 分析页签（统计/直方图等分支挂载；首次添加才显示）----
    void addAnalysisTab( const QString &title, QWidget *w );

    // ---- D4.7/D3.7 记忆键 ----
    void setAssetKey( const QString &key );
    QString assetKey() const { return m_assetKey; }

    // ---- D3.7 书签 ----
    int bookmarkCount() const;
    void addBookmarkNamed( const QString &name );          // 存当前视图
    bool jumpToBookmark( const QString &name );
    bool removeBookmark( const QString &name );

    // ---- D3.9 鹰眼 ----
    void setOverviewVisible( bool on );
    bool overviewVisible() const;

    // ---- D1.5 装饰件注册式开关（scaleBar/northArrow/grid）----
    void setDecorationEnabled( const QString &name, bool on );
    bool decorationEnabled( const QString &name ) const;

    // ---- 状态条读数（测试断言面）----
    QString coordinateReadout() const;
    QString scaleReadout() const;
    QString renderReadout() const;
    QString measureReadout() const;

    // ---- D6.1 渐进渲染第一段：低清整图先上屏 ----
    void showLowResSnapshot();

    // ---- D6.2/D6.6 渲染缓存：identity 设置后，renderCompleted 自动存；
    // primeRenderCache() 命中即上 overlay（首次打开 <300ms 的主力路径）。----
    void setRenderCacheIdentity( const QString &assetId, const QString &versionId );
    void primeRenderCache();

    // 分支扩展工具条（survey 的工区范围/切主画布等专属按钮）。
    void addToolBarAction( QAction *action );
    void addToolBarSeparator();
    void addToolBarWidget( class QWidget *widget );

  signals:
    // 剖面线拖出（D5.1）：分支采样后 profilePanel()->addProfile(...)。
    void profileLineDrawn( const QgsPointXY &p1, const QgsPointXY &p2, int totalLines );
    // 测区全景「在主画布中查看」等壳意图。
    void requestShowOnMainCanvas();
    void mapLayersChanged();

  private:
    void buildToolBar();
    void buildStatusBar();
    void rebuildBookmarkMenu();
    void updateNavActions();
    void runIdentifyAt( const QgsPointXY &point );
    void runIdentifyRect( const QgsRectangle &rect );
    void copyCoordinate();
    void copyScreenshot();

    // 内嵌鹰眼（小画布 + 视口框 + 拖动跟随）。
    class PreviewOverviewMap;
    friend class PreviewOverviewMap;
    PreviewOverviewMap *m_overview = nullptr;

    PreviewMapCanvas *m_canvas = nullptr;
    PreviewMapToolManager *m_tools = nullptr;
    PaleoDecorationManager *m_decor = nullptr;
    PreviewTocPanel *m_toc = nullptr;
    PreviewIdentifyPanel *m_identify = nullptr;
    PreviewProfilePanel *m_profile = nullptr;
    PreviewIdentifyCore *m_identifyCore = nullptr;

    QToolBar *m_toolBar = nullptr;
    QStackedWidget *m_mapStack = nullptr;
    QWidget *m_errorPage = nullptr;
    QTabWidget *m_sideTabs = nullptr;
    QTabWidget *m_analysisTabs = nullptr;
    QWidget *m_statusBar = nullptr;
    QLabel *m_renderLabel = nullptr;
    QLabel *m_coordLabel = nullptr;
    QLabel *m_scaleLabel = nullptr;
    QLabel *m_measureLabel = nullptr;
    QMenu *m_bookmarkMenu = nullptr;

    QAction *m_panAction = nullptr;
    QAction *m_zoomInAction = nullptr;
    QAction *m_zoomOutAction = nullptr;
    QAction *m_identifyAction = nullptr;
    QAction *m_measureLineAction = nullptr;
    QAction *m_measureAreaAction = nullptr;
    QAction *m_profileAction = nullptr;
    QAction *m_backAction = nullptr;
    QAction *m_fwdAction = nullptr;
    QActionGroup *m_toolGroup = nullptr;
    QHash<QString, QAction *> m_toolActions;

    QString m_assetKey;
    QString m_cacheAssetId;
    QString m_cacheVersionId;
    QgsPointXY m_lastMousePos;
    bool m_hasMousePos = false;
};
