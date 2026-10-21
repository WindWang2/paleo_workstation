// 层：QGIS 封装
#pragma once

#include <QDomDocument>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <memory>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransformcontext.h>
#include <qgsfeature.h>

class QgsProject;
class QgsLayout;
class QgsPrintLayout;
class QWidget;
class QgisLayerService;
class QgisProjectService;

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

// Capture declarations on their owner thread; reconstruct private QGIS objects
// in the worker. Memory features are copied out as values on the owner thread —
// the worker never touches the live layer's provider.
struct ProjectSnapshot {
  QDomDocument xml;
  QHash<QString, QgsFeatureList> memoryFeatures;
  QgsCoordinateReferenceSystem crs;
  QgsCoordinateTransformContext transformContext;
  QString ellipsoid, title, homePath;
  QVariantMap customVariables;
};
bool captureProjectSnapshot(const QgsProject *project, ProjectSnapshot &out,
                            QString *reason = nullptr);
// rebuilds the private worker project from a snapshot. error is only set when
// the project itself cannot be assembled; individual layers whose source is
// unreadable are kept as invalid layers (same semantics as the live project)
// and appended to *warnings as "name: reason" entries — an export that
// silently drops layers is not a clean success.
std::unique_ptr<QgsProject> rebuildProject(const ProjectSnapshot &snapshot,
                                           QString *error = nullptr,
                                           QStringList *warnings = nullptr);

// 图件版面句柄（#531）：QgsPrintLayout 的所有权关在封装层内——对话框的
// Builder 与 geologicalexport::start 只传递句柄，功能层/视图层的签名与成员
// 不再出现 Qgs* 类型。
class LayoutHandle {
public:
  explicit LayoutHandle(std::unique_ptr<QgsPrintLayout> layout);
  ~LayoutHandle();
  QgsPrintLayout *layout() const { return m_layout.get(); }
private:
  std::unique_ptr<QgsPrintLayout> m_layout;
};

// 工程句柄（#531）：与 LayoutHandle 对称——worker 侧重建的私有 QgsProject
// 所有权也关在封装层内。功能层（geologicalexport）不 include QGIS 头，
// QgsProject 只有前向声明，持 unique_ptr<QgsProject> 连析构都编不过
// （不完整类型 C2338）；析构留在本封装层的 TU 里。
class ProjectHandle {
public:
  explicit ProjectHandle(std::unique_ptr<QgsProject> project);
  ~ProjectHandle();
  QgsProject *project() const { return m_project.get(); }
private:
  std::unique_ptr<QgsProject> m_project;
};

// 一次性快照：工程快照 + 版面 XML + 图面（地图项）引用的图层 id。worker 依此
// 在私有工程上重建版面，功能层不再触碰 QgsPrintLayout / QgsReadWriteContext /
// QgsLayoutItemMap（#531）。
struct LayoutSnapshot {
  ProjectSnapshot project;
  QDomDocument xml;
  QStringList requiredLayerIds;
};
bool captureLayout(QgsPrintLayout *layout, LayoutSnapshot &out, QString *error = nullptr);
// 在已重建的私有工程上按 XML 还原版面（readXml 失败 → nullptr + *error）。
// 版面所有权装进 LayoutHandle：功能层只见句柄，QgsPrintLayout 的析构留在
// 封装层（同 ProjectHandle 口径，否则不完整类型销毁即 C2338）。
std::unique_ptr<LayoutHandle> rebuildLayout(QgsProject *project, const QDomDocument &xml,
                                            QString *error = nullptr);
// rebuildProject 的句柄化入口（#531）：error/warnings 口径与 rebuildProject
// 一致，只是把 QgsProject 所有权装进 ProjectHandle 交给功能层。
std::unique_ptr<ProjectHandle> rebuildProjectHandle(const ProjectSnapshot &snapshot,
                                                    QString *error = nullptr,
                                                    QStringList *warnings = nullptr);
// 图面引用的图层是否都可用（缺失/数据源损坏即失败，错误带图层 id）。
bool validateLayoutLayers(QgsPrintLayout *layout, const QStringList &requiredLayerIds,
                          QString *error = nullptr);
// 版面模板落盘（QgsReadWriteContext 由封装层管）。
bool saveLayoutTemplate(QgsPrintLayout *layout, const QString &path, QString *error = nullptr);
// QGIS 运行库版本串（metadata.json 的 qgis_version 字段来源；功能层不再
// 直接 include <qgis.h> 取 Qgis::version()）。
QString qgisVersion();

enum class Format { Png, Pdf, Svg, Tiff };
bool formatAvailable(Format format);
QString formatExtension(Format format);
bool geoPdfAvailable(QString *reason = nullptr);
struct ExportOptions {
  bool transparentBackground = false;
  bool worldFile = false;
  bool geoPdf = false;
  bool textAsPaths = true;
};

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
    QStringList warnings; //!< non-fatal issues (e.g. snapshot layers whose data
                          //!< source was unreadable → map rendered without them)
    QStringList auxiliaryFiles; //!< world files and CRS sidecars
};

// Logic core — synchronous, no dialogs, directly testable.
//   layout   the layout to export (must not be null)
//   outPath  destination; a default extension (.png/.pdf/.svg) is appended
//            when the path has none
//   dpi      export resolution; <= 0 keeps the layout's own dpi
//   range    page selection (see PageRange); pass PageRange() for all pages
ExportOutcome exportLayout( QgsLayout *layout, const QString &outPath, Format format,
                            double dpi, const PageRange &range,
                            const ExportOptions &options = {} );
// worker 侧入口（#531）：rebuildLayout 重建出来的是 QgsPrintLayout，功能层
// （geologicalexport/horizonbatchexport）不再 include QGIS 头，拿不到
// QgsLayout 的完整定义做基类转换——与 validateLayoutLayers/saveLayoutTemplate
// 同口径收一把 QgsPrintLayout * 重载，转调上面的 QgsLayout * 实现。
ExportOutcome exportLayout( QgsPrintLayout *layout, const QString &outPath, Format format,
                            double dpi, const PageRange &range,
                            const ExportOptions &options = {} );
// Builds the thickness map print layout on project (A4 landscape with map, title, legend, scalebar, north arrow, CRS caption).
QgsPrintLayout *buildHorizonMapLayout( QgisLayerService *layers, QgisProjectService *projectSvc,
                                       const QString &horizon, QString *error = nullptr );

// ---- 版面预览小部件（#531 薄壳的工厂面）----
// PaleoLayoutPreview（layoutpreview.h）是 QgsLayoutView 的 QWidget 薄壳，其
// 头必含 <QWidget>——视图层直接 include 即向 ui_qtwidgets_notes 传递债记一笔
// （tools/transitive-baseline.txt 头注：#533 差额 1 条）。与
// qgisprocessingservice.h 的 createAlgorithmDialog 同口径：头文件只留 QWidget
// 前向声明，消费 TU 不再被迫吃 QtWidgets；定义在 .cpp（回收本厂产物需完整
// 类型）。两个操作都是 no-op 守卫（preview 非本厂产物时安全返回）。
QWidget *createLayoutPreview( QWidget *parent = nullptr );
void setLayoutPreviewLayout( QWidget *preview, QgsPrintLayout *layout );
void zoomLayoutPreviewFull( QWidget *preview );

} // namespace PaleoLayoutExport
