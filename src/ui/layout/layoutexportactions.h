// 层：视图
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

#include "../../qgis/layoutexport.h" // Format/PageRange/ExportOutcome + 导出核心

class QAction;
class QStatusBar;
class QgsLayout;
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

    // #85：注入任务服务后 action 导出走 worker——版面先存 XML 快照（GUI
    // 线程、毫秒级），worker 重建私有 QgsLayout 再跑导出器，1200dpi 多页
    // 不再卡主线程/泵事件。快照语义：导出内容是点击时刻的版面，编辑中的
    // 后续改动不进该次导出。未注入时保持同步旧路径（测试壳/无服务宿主）。
    void setTaskService( PaleoTaskService *service );

    // 任务池导出路径（对话框之外的可测缝）：snapshot → worker 重建导出，
    // 完成时 emit exportFinished + 状态条/打开文件夹由 finished 回包处理。
    // 返回 false = 无任务服务或版面为空（调用方回退同步路径）。
    bool exportLayoutAsync( QgsLayout *layout, const QString &outPath, Format format,
                            double dpi, const PageRange &range );

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
    // worker 导出结果暂存：worker 写、finished 回包（GUI）读。
    ExportOutcome m_pendingOutcome;
    void reportExportOutcome( const ExportOutcome &outcome );
};
