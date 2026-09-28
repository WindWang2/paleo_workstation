// 层：QGIS 封装
#pragma once

#include <QString>
#include <QStringList>

class QgsLayout;

// qgis/layoutexport — QgsLayoutExporter 的页选/多文件导出封装（QGIS 封装层）。
// 从 ui/layout/layoutexportactions 抽出的逻辑核心：纯同步调用，无对话框、
// 无事件循环——UI 壳（layoutexportactions）与功能层（workflow/mapexport）
// 共用这一份导出管线。
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
namespace PaleoLayoutExport
{

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
    QString effectivePath; //!< destination after default-extension resolution; empty
                           //!< when layout==nullptr or outPath empty (pre-resolution errors)
};

// Logic core — synchronous, no dialogs, directly testable.
//   layout   the layout to export (must not be null)
//   outPath  destination; a default extension (.png/.pdf/.svg) is appended
//            when the path has none
//   dpi      export resolution; <= 0 keeps the layout's own dpi
//   range    page selection (see PageRange); pass PageRange() for all pages
ExportOutcome exportLayout( QgsLayout *layout, const QString &outPath, Format format,
                            double dpi, const PageRange &range );

} // namespace PaleoLayoutExport
