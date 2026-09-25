#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <functional>

class QAction;
class QStatusBar;
class QgsLayout;

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
    enum class Format { Png, Pdf, Svg };

    // 0-based page selection, QGIS convention (page 0 = first page).
    // fromPage/toPage are inclusive and clamped to the existing page count;
    // a range that selects no pages at all is reported as an error.
    struct PageRange
    {
        enum class Mode { All, Current, Range };
        Mode mode = Mode::All;
        int currentPage = 0; //!< used when mode == Current
        int fromPage = 0;    //!< used when mode == Range (inclusive)
        int toPage = 0;      //!< used when mode == Range (inclusive)
    };

    struct ExportOutcome
    {
        bool ok = false;
        QString error;     //!< human-readable, empty on success
        QStringList files; //!< files written: primary path first, then "_2".. siblings
    };

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
};
