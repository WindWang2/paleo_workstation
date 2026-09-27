#pragma once
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QVector>
#include <functional>

#include "../io/dataimportservice.h"   // FolderPreviewRow / FolderRowResult（T22 静态面）

class QComboBox;
class QDialog;
class QLabel;
class QTableWidget;

class QgisCanvasController;
class QgisProjectService;
class QgisLayerService;
class ToolAvailabilityService;
class SelectionContext;
class QgsMapLayer;
class QTabBar;
class QDockWidget;
class QStackedWidget;
class QSplitter;
class DataPreviewTabs;
class PredictionWorkflow;
class ConstraintWorkflow;
class CompositionWorkflow;
class ValidationWorkflow;
class MappingWorkflow;
class MapVersionController;
class MapVersionStore;
class ProjectDataFacade;
class DataCatalog;
class DataImportService;
class SeismicMapLink;
class QgisProcessingService;
class QgisLayoutService;
class QgisEditingService;
class PaleoProjectStore;
class PaleoTaskService;
class PaleoDecorationManager;
class QCloseEvent;

// ui/ — PaleoMainWindow: the five-page workflow shell (§42).
// Anatomy: left = layer tree dock; center = canvas (+ startup page stacked under);
// right = page-specific dock; bottom = log/tasks tabs; top = workflow chain tab bar.
// UI never touches Qgs* beyond canvas/layer-tree widgets (§25).
class PaleoMainWindow : public QMainWindow
{
  Q_OBJECT
  public:
    PaleoMainWindow(QgisCanvasController *canvasCtl,
                    QgisProjectService *projectSvc,
                    QgisLayerService *layerSvc,
                    ToolAvailabilityService *tools,
                    SelectionContext *selection,
                    QWidget *parent = nullptr);

    // Page ids: "data" | "predict" | "constraint" | "compose" | "validate" (+ "startup")
    void showPage(const QString &pageId);
    QString currentPage() const { return m_currentPage; }
    void showStartup();            // first-run: recent projects + new/open
    void onProjectOpened();        // called after project opens: swap startup->workspace

    // §42 shell persistence — geometry/dock layout/last page in QSettings;
    // canvas extent is per-project and lives inside the .qgz (paleo props).
    void saveWindowState();
    void restoreWindowState();
    void saveCanvasExtent();       // write current canvas extent into the project
    void restoreCanvasExtent();    // apply stored extent after project opens

    // Swap right-dock placeholder panels for the real page panels (§42.2),
    // bound to the workflow orchestrators. Call after AppContext assembly.
    void attachWorkflows(PredictionWorkflow *pred, ConstraintWorkflow *constraint,
                         CompositionWorkflow *compose, ValidationWorkflow *validate,
                         DataImportService *importSvc = nullptr,
                         SeismicMapLink *seismicLink = nullptr,
                         QgisProcessingService *procSvc = nullptr,
                         PaleoProjectStore *store = nullptr,
                         QgisEditingService *editSvc = nullptr,
                         QgisLayoutService *layoutSvc = nullptr,
                         PaleoTaskService *taskSvc = nullptr);

    // wave/mapping-pipeline 阶段C+E：编图链 / 层位图导出 / 版本状态机接到
    // ③编图页。独立于 attachWorkflows，避免动其签名。Call after attachWorkflows.
    // catalog 用于导出产物登记（OUTPUT 资产，发布门的前提）。
    void attachMapping(MappingWorkflow *mapping, MapVersionController *versions,
                       MapVersionStore *versionStore, ProjectDataFacade *projectData,
                       DataCatalog *catalog = nullptr);

    // ---- T22 文件夹确认表（静态面，tst_panels 直接驱动；runFolderImport 只
    // 负责选目录 + exec）----
    // 类型下拉的稳定 label↔type 映射：label 只管显示，type 存 Qt::UserRole，
    // 永不靠显示文本反推。词表 = 分类器实际输出集（含 tabular，无「tops」——
    // 那是关联角色）+ reference 伪类型。
    static QString folderTypeLabel(const QString &type);
    // 行默认显示类型：HZ28-6-1 固定辅助 → 「参考」；「参考资料」目录内井类/
    // 未判内容默认「参考」（可改）；其余行显示分类器原类型。
    static QString folderRowDisplayType(const QString &path, const QString &classifiedType);
    // 建确认表行（锁定行禁用下拉 + tooltip、跳过行灰显）；combosOut 收每行下拉。
    static void populateFolderConfirmTable(
        QTableWidget *table, const QString &rootDir,
        const QVector<DataImportService::FolderPreviewRow> &rows,
        QVector<QComboBox *> *combosOut);
    // 覆盖收集：只看启用行；选中映射类型合法且不同于分类器原类型才成 override。
    static QMap<QString, QString> collectFolderTypeOverrides(
        const QTableWidget *table, const QVector<DataImportService::FolderPreviewRow> &rows,
        const QVector<QComboBox *> &combos);
    // 行结果写回（实体列 + 结果列）；Failed 且给了 onRetry → 结果列挂「重试」按钮。
    static void writeFolderRowResult(QTableWidget *table, int row,
                                     const DataImportService::FolderRowResult &res,
                                     const std::function<void(int)> &onRetry);
    // 汇总文案：「入库 n，未决 n，失败 n（，跳过 n）」——D3 保留第四计数。
    static QString folderImportSummaryText(
        const QVector<DataImportService::FolderRowResult> &rows);
    // 确认对话框整体搭建（类型表 + CRS 说明句 + 确认/取消 + 行重试接线）。
    // self 可为空（测试）；非空时用于文件夹导入期的预览抑制与井口标签打开。
    static void buildFolderConfirmDialog(
        QDialog *dlg, DataImportService *svc, const QString &dir,
        const QVector<DataImportService::FolderPreviewRow> &preview,
        PaleoMainWindow *self);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    void buildShell();
    // 预览分栏（§4 预览壳）：数据页地图在上预览在下；预览空态收成一行
    // 次级文字，首个标签打开时展开到约三分之一高度。
    void applyPreviewSplit();
    // 「导入工区文件夹」：分类确认表（可改类型）→ 两阶段导入 → 计数汇总，
    // 确认后只打开井口标签（§3/autoplan-design）。
    void runFolderImport(DataImportService *svc);
    // 已知目录的入口变体（「从工区文件夹新建」复用同一确认框流程）。
    void runFolderImportAt(DataImportService *svc, const QString &dir);
    // PROJECT_FILE_DESIGN：文件夹导入完成后把来源+统计回填 project.paleo
    // 的 sourceArea。只在「工程目录==导入目录」（从文件夹新建的工程）时写。
    void stampSourceArea(const QString &dir, const QVariantMap &stats);
    // D11 临时配准：手工仿射 → DERIVED GeoJSON 版本登记 → 「临时配准 · 名」
    // 矢量图层实例化 + 画布水印；失败走状态栏文案，不弹框。
    void applyProvisionalRegistration(DataImportService *svc,
                                      const QString &assetId,
                                      const QVariantMap &params);
    // T29 闪烁定位：图层范围上盖一条主色半透明橡皮带，100ms 一闪 ×4
    // （共 400ms，spec ~300–500ms）后自毁。只动橡皮带不动图层可见性——
    // 不与「在地图上显示」的双向同步打架。测试经 "horizonFlashActive"
    // 属性断言起止。
    void flashHorizonLayer(QgsMapLayer *layer);

    QgisCanvasController *m_canvasCtl;
    QgisProjectService *m_projectSvc;
    QgisLayerService *m_layerSvc;
    ToolAvailabilityService *m_tools;
    SelectionContext *m_selection;

    // 阶段E 发布门重算钩子：attachMapping 安装，attachWorkflows 的
    // validationDone 连接在验证跑完后调它（残差覆盖是门的一条腿）。
    std::function<void()> m_refreshPublishGate;

    QTabBar *m_workflowTabs = nullptr;
    QStackedWidget *m_centerStack = nullptr;   // page0=startup, page1=map+preview
    QSplitter *m_centerSplit = nullptr;        // 地图 / 预览 竖向分栏（§4）
    DataPreviewTabs *m_previewTabs = nullptr;  // 分栏下格——只在数据管理页可见
    bool m_previewExpanded = false;            // 首个标签打开后已给过 60%（D7 预算）
    bool m_previewMaximized = false;           // D7：预览最大化态（地图留 64px）
    QList<int> m_preMaxSplitSizes;             // 最大化前的分栏尺寸（还原用）
    QDockWidget *m_leftDock = nullptr;
    QDockWidget *m_rightDock = nullptr;
    QDockWidget *m_bottomDock = nullptr;
    bool m_folderImportActive = false; // 文件夹导入期间抑制逐文件开预览标签
    PaleoTaskService *m_taskSvc = nullptr; // attachWorkflows 注入；空 → 导入走同步旧路径
    DataImportService *m_importSvc = nullptr; // attachWorkflows 注入；启动页「从工区文件夹新建」用
    // attachWorkflows 幂等守卫：该函数每次执行都清栈重建右栏页面、给底栏/
    // 状态栏加面板并往服务对象上叠信号连接，二次执行会重复建 dock/按钮并
    // 遗留悬空引用（后续用例段错误）。测试套件会二次触达同一窗口——
    // 入口早退，见 attachWorkflows 注释。
    bool m_workflowsAttached = false;
    PaleoDecorationManager *m_decorMgr = nullptr; // D11 临时配准水印等画布装饰
    int m_provisionalLayers = 0;   // 已上图的临时配准图层数（>0 → 水印）
    QString m_currentPage;
};
