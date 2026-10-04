// 层：视图
#pragma once
#include <QDomDocument>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <memory>

#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransformcontext.h>
#include <qgsfeature.h>

#include "../../qgis/layoutexport.h" // Format/PageRange/ExportOutcome + 导出核心

class QAction;
class QStatusBar;
class QgsLayout;
class QgsProject;
class PaleoTask;
class PaleoTaskService;

// ui/layout — Task C: export actions for the bespoke layout designer shell.
//
// Two layers, deliberately split so the export pipeline is testable without
// dialogs or an event loop:
//   * logic core — exportLayout(): pure QgsLayoutExporter call, synchronous,
//     returns an ExportOutcome (ok/error/files) and emits exportFinished();
//   * UI path — exportPng/Pdf/SvgAction() → file dialog → settings dialog
//     (dpi QSpinBox 72–1200 default 300, page range controls) → core export →
//     status bar message (if setStatusTarget() was given) → optional
//     "open containing folder" (only when setOpenFolderEnabled(true)).
//
// Page selection reality on QGIS 4.2 (verified against qgslayoutexporter.h and
// probed on 4.2.2):
//   * ImageExportSettings has a native 0-based `pages` list, but output files
//     are named by ORIGINAL page number (exporting only page 1 produces
//     "base_2.png") — so PNG uses the native list only for contiguous
//     selections starting at the first page; every other selection (and all
//     Pdf/Svg selections, whose settings have no page list at all) goes
//     through a renumbered, trimmed clone of the layout. The source layout is
//     never modified, and outPath always receives the first selected page.
//   * multi-page PNG/SVG exports write `base.ext` + `base_2.ext`, ... — the
//     siblings are reported back in ExportOutcome::files.
class PaleoLayoutExportActions : public QObject
{
    Q_OBJECT
  public:
    // 导出核心类型已抽到 qgis/layoutexport.h（QGIS 封装层）；别名保留——
    // 调用点（mapexport / tst_layoutexport / designer shell）写法不变。
    using Format = PaleoLayoutExport::Format;
    using PageRange = PaleoLayoutExport::PageRange;
    using ExportOutcome = PaleoLayoutExport::ExportOutcome;

    explicit PaleoLayoutExportActions( QObject *parent = nullptr );

    QAction *exportPngAction();
    QAction *exportPdfAction();
    QAction *exportSvgAction();

    // Logic core — synchronous, no dialogs, directly testable.
    //   layout   the layout to export (must not be null)
    //   outPath  destination; a default extension (.png/.pdf/.svg) is appended
    //            when the path has none
    //   dpi      export resolution; <= 0 keeps the layout's own dpi
    //   range    page selection (see PageRange); pass PageRange() for all pages
    // Returns the outcome and emits exportFinished(effectivePath, ok).
    ExportOutcome exportLayout( QgsLayout *layout, const QString &outPath, Format format,
                                double dpi, const PageRange &range );

    // #85/#161：注入任务服务后 action 导出走 worker。GUI 线程只做序列化
    // （版面 XML + 工程/图层 XML 快照，毫秒级）；worker 用快照重建一个
    // **私有** QgsProject（图层/图层树/地图主题都在 worker 上新建、归 worker
    // 线程所有）和私有 QgsPrintLayout 再跑导出器——渲染作业的
    // createMapRenderer() 只碰 worker 自己的图层，不再与 GUI 线程上的活图层/
    // 样式/工程切换并发（#161）。快照语义：导出内容是点击时刻的版面与样式。
    // 未注入时保持同步旧路径（测试壳/无服务宿主）。
    void setTaskService( PaleoTaskService *service );

    // 任务池导出路径（对话框之外的可测缝）。返回值：
    //   true  = 已受理——要么已排队（完成时 exportFinished + 状态条回包），
    //           要么因已有导出进行中被拒（立即 exportFinished(outPath,false)）；
    //   false = 无法隔离/无任务服务/版面为空，调用方应回退同步路径；
    //           fallbackReason（可空）写入原因。
    bool exportLayoutAsync( QgsLayout *layout, const QString &outPath, Format format,
                            double dpi, const PageRange &range,
                            QString *fallbackReason = nullptr );

    //! 本实例是否有任务池导出在跑（期间导出动作禁用、再次导出被拒）。
    bool exportInFlight() const { return m_inFlight; }

    // #161 工程隔离快照：GUI 线程 capture，worker 线程 rebuild。值语义——
    // 抓取后与源工程/图层再无共享对象（QDomDocument 由本快照独占）。
    struct ProjectSnapshot
    {
        QDomDocument xml; //!< <qgis><projectlayers/><layertree/><visibility-presets/></qgis>
        QHash<QString, QgsFeatureList> memoryFeatures; //!< memory 图层的要素（XML 不含要素）
        QgsCoordinateReferenceSystem crs;
        QgsCoordinateTransformContext transformContext;
        QString ellipsoid;
        QString title;
        QString homePath;
        QVariantMap customVariables;
    };
    // 失败（插件图层、带未提交编辑的图层、序列化失败）返回 false + reason：
    // 这些情形无法在 worker 上忠实重现，调用方回退 GUI 线程同步导出。
    static bool captureProjectSnapshot( const QgsProject *project, ProjectSnapshot &out,
                                        QString *reason = nullptr );
    // 在**调用线程**新建私有工程并按快照重建；返回的工程及其图层都归调用
    // 线程所有。失败返回 nullptr + error。
    static std::unique_ptr<QgsProject> rebuildProject( const ProjectSnapshot &snapshot,
                                                       QString *error = nullptr );

    // UI wiring. The providers are only consulted by the action handlers —
    // exportLayout() itself takes its layout explicitly.
    void setLayoutProvider( const std::function<QgsLayout *()> &provider );
    void setCurrentPageProvider( const std::function<int()> &provider ); //!< 0-based
    void setStatusTarget( QStatusBar *status );
    void setOpenFolderEnabled( bool enabled ); //!< default false (tests keep it off)

  signals:
    void exportFinished( const QString &path, bool ok );

  private:
    void runExportUi( Format format );

    QAction *m_pngAction = nullptr;
    QAction *m_pdfAction = nullptr;
    QAction *m_svgAction = nullptr;

    std::function<QgsLayout *()> m_layoutProvider;
    std::function<int()> m_currentPageProvider;
    QPointer<QStatusBar> m_statusTarget;
    bool m_openFolderEnabled = false;

    PaleoTaskService *m_taskSvc = nullptr;
    // #161：结果按任务携带（shared_ptr 进 work 与 finished 回包），不再有
    // worker 写 GUI 对象成员的单槽；m_inFlight 只在 GUI 线程读写。
    bool m_inFlight = false;
    void setInFlight( bool inFlight );
    void reportExportOutcome( const ExportOutcome &outcome );
};
