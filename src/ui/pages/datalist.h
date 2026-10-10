// 层：视图
#pragma once

#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <memory>

#include "dataops/dataopscommands.h"
#include "dataops/dataopsfilter.h"
#include "dataops/dataopsfuzzy.h"
#include "dataops/dataopsmodel.h"
#include "dataops/dataopsselection.h"
#include "dataopsundo.h"
#include "datanavtree.h"

#include "../../services/cataloghealth.h"

class PreviewDocService;
class QStackedWidget;
class CatalogHealthDialog;
class StorageGovernanceController;
class StorageGovernanceDialog;
class AssetEntityChoiceModel;

namespace paleo::dataops
{
class ImportQueuePanel;
class AssetIconView;
class AssetVirtualView;
class AssetGroupTree;
class FilterBar;
class FilterChipBar;
class PendingQuickBar;
class TagCloudWidget;
class FilterEmptyState;
class SelectionBadge;
class HighlightDelegate;
class OperationsHistory;
struct VersionRow;
} // namespace paleo::dataops

// ui/pages/datalist — 数据管理页的「导入 + 列表」侧（W5：自 DataPage 分家；
// P3 数据操作重构：多选/过滤/拖拽/批量/撤销/视图形态全部在本面板落地）。
//
// 含两段（objectName 不变）：dataImportSection（导入按钮）/ dataListSection
//（搜索 + 类型筛选 + 未决过滤条 + 树/表 viewStack）。数据访问只走
// PreviewDocService 门面（壳经 DataPage 动态属性下发 setDocService）。
// 信号签名与旧 DataPage 一致（薄壳转发到壳）。
class DataListPanel : public QWidget
{
  Q_OBJECT

  public:
  // 方向34：计划井可见性过滤器（壳注入——siting 文档 retired 面与数据页
  // 树保持一致；空 = 不过滤）。返回 false 的实体不出「计划井」组。
  void setPlannedVisibilityFilter( std::function<bool( const QString &id )> visible )
  {
    m_plannedVisible = std::move( visible );
  }

    explicit DataListPanel(QWidget *parent = nullptr);

    // 门面下发（DataPage 的 "paleo.page.importsvc" 动态属性 → 本面板）。
    void setDocService(PreviewDocService *doc);

    // ---- D1/D5 共享面（DataPage 转发给 EntityPanel，实体侧操作共用栈/存储） ----
    paleo::dataops::DataOpsUndoStack *opStack() const { return m_opStack; }
    // B2（wave/deepen-perf）：导入队列面板——壳接生产 runner
    //（FolderImportQueueAdapter）用；队列未建时为 nullptr。
    paleo::dataops::ImportQueuePanel *importQueuePanel() const { return m_importQueue; }
    const paleo::dataops::DataOpsContext &opsContext() const { return m_ctx; }
    paleo::dataops::OperationsHistory *operationsHistory() const { return m_history.get(); }
    // D6.2 命令注册表（DataPage 装配命令面板时读取）。
    const paleo::dataops::CommandRegistry &commandRegistry() const { return m_reg; }

    // ---- D1 选中面 ----
    QSet<QString> currentAssetSelection() const;      // 表+树聚合
    QStringList currentEntitySelection() const;       // 树中井节点
    paleo::dataops::SelectionMix currentSelectionMix() const;

    // ---- D2 过滤面 ----
    paleo::dataops::FilterGroup filterGroup() const { return m_filter; }
    void applyFilterGroup(const paleo::dataops::FilterGroup &g); // 替换并应用
    QString filterStateString() const { return m_filter.toStateString(); }
    void setFilterFromStateString(const QString &s);
    QString activeTagFilter() const { return m_activeTag; }

    // ---- 树排序（D2.5）----
    void setTreeSort(paleo::dataops::TreeSortKind kind);

    // ---- 方向 92：树增量通道诊断面 ----
    enum class TreeRefreshMode
    {
      Unchanged,    // 蓝图与现状零差异（不 clear、不碰展开/滚动）
      Incremental,  // 节点级增/删/改就地落地
      FullRebuild   // 变更面超阈值 → 全量兜底（#270 快照还原语义）
    };
    TreeRefreshMode lastTreeRefreshMode() const { return m_lastTreeRefreshMode; }
    // 增量兜底阈值：节点级变更数（增+删+改）超过 max(下限, 既有节点数/除数)
    // → 全量重建（结构性重排/大批量导入走这里）。口径定案见 ledger 方向 92。
    static constexpr int kNavReconcileFloor = 16;
    static constexpr int kNavReconcileDivisor = 4;

    // ---- D1.9 ----
    // 命令面（测试/编程式操作入口；右键菜单与拖放共用）。
    void pushCommand(paleo::dataops::DataOpCommand *cmd);

  public slots:
    void selectAllVisibleAssets();
    void invertAssetSelection();
    void selectByCurrentFilter(); // 按过滤器选中（全选可见项的同义词，保留语义口）

    void refreshAssetTable();   // 从 catalog 资产重建资产表/树
    void applyListFilter();     // 名称/类型/关联含搜索词且类型匹配的行才显示
    // T31「查看未决」：资产表过滤到仍有未决链接的行；off 清除过滤。
    void setUnresolvedFilter(bool on);
    // D6 地图→表联动：实体 id 集合 → 选中这些实体已决关联对应的资产行
    // （滚动到首个命中行；行选中照发 assetActivated）。无命中不改当前选中。
    void selectAssetsForEntities(const QStringList &entityIds);
    // 表 + 树定位到指定资产（不发激活信号——setCurrentCell 走 QSignalBlocker）。
    void selectAssetInViews(const QString &assetId);

    // ---- D5 撤销/重做（含状态反馈信号）----
    void undoOp();
    void redoOp();

    // ---- D1 批量操作（右键菜单/命令面板共用入口）----
    void batchAttachToEntity();          // D1.4 目标实体对话框
    void batchChangeType();              // D1.5（确认 + 失败明细）
    void batchRemoveSoft();              // D1.6 软删 → 可回收清单
    void batchExportManifest();          // D1.7 CSV/JSON
    void batchOpenPreview();             // D1.8 前几项进标签
    void batchAddTag();                  // D2.4 选中打标签
    void showRecycleBin();               // D1.6 可回收清单对话框
    // ---- 方向 30：工区数据管线与健康管理 ----
    void showImportLedger();             // 导入台账查看器（批次 + 行级结局）
    void showStorageGovernance();
    void showHealthCheck();              // 资产体检对话框（快速面 + SHA 复验）
    void showVersionTable();             // 单资产版本面对话框（对比 + 回滚）
    void resolvePendingLinks();          // 未决链接批量归位（恰好一候选才挂）
    // ---- 单资产操作 ----
    void detachSingleAssetLink();        // 解挂（首个选中资产的已决链接）
    void setPrimaryForSelection();       // 设为主版本
    void editRoleForSelection();         // D4.7（不可撤销确认）
    void editDepthAnchorForSelection();  // 方向 79：图片附件锚深后补编辑
    // ---- 方向 79：井附件管理 ----
    void showWellAttachments(const QString &wellId); // 树「岩心照片」双击入口
    void removeAssetsSoft(const QStringList &assetIds); // 软删执行面（树/面板共用）
    // ---- D3.2 外部文件拖入 → 导入队列 ----
    void handleExternalFiles(const QStringList &paths);
    // ---- D7 视图形态 ----
    void setViewMode(int mode);          // 0 树 1 表 2 图标 3 虚拟 4 分组
    void openColumnConfig();             // D7.2
    void openGroupConfig();              // D7.6 分组维度

  signals:
    void importRequested(const QString &kind);   // "wells" | "seismic" | "boundary" | ...
    void versionActivated(const QString &versionId); // 体检 stale 版本定位
    void assetActivated(const QString &assetId); // 列表选中 → 预览标签打开
    // goal/gridding-surface-ops：层位散点 → 网格化参数表（壳接线异步任务）。
    void gridHorizonRequested(const QString &assetId);
    // 方向 32：井分层资产的编辑入口（壳接 WellTopsEditorDialog）。
    void topsEditRequested(const QString &assetId);
    // 树节点关联井选中 → 预览打开并定位到该井
    void assetWellActivated(const QString &assetId, const QString &wellId);
    void seismicLineActivated(const QString &assetId, const QString &mode);
    void wellSelected(const QString &wellId); // 树中选中井 → 地图高亮
    void surveyAreaActivated();               // 双击测区 → 测区全景地图
    // 内部建/撤挂接变更 catalog 后请实体视图重取（薄壳接 EntityPanel::refresh）。
    void entityRefreshRequested();
    // 列表/树内选中资产或实体——薄壳转回 DataPage::selectAsset /
    // selectAssetsForEntities（实体视图定位口径由壳统一）。
    void assetFocusRequested(const QString &assetId);
    void entitiesFocusRequested(const QStringList &entityIds);
    // ---- P3 新增信号 ----
    void selectionCountChanged(int assetCount, int entityCount); // D1.2
    void statusMessage(const QString &msg);         // D5.4/D8 反馈（壳接状态栏）
    void externalImportRequested(const QStringList &paths); // D3.2（壳接导入流）
    // D4.6 实体操作经意图信号（实体编辑对话框在 EntityPanel 侧有完整版；
    // 树内入口给轻量路径）。
    void entityRenameRequested(const QString &entityId);
    void entityDeleteRequested(const QString &entityId);
    void shortcutsDialogRequested(); // D6.3（DataPage 接快捷键表对话框）
    // 方向 47：集合树成员/统计面激活 → 壳侧物化 realset.* 图层上图。
    void realizationMemberRequested(const QString &setId, int index);
    void realizationStatRequested(const QString &setId, const QString &token);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    // 方向34：计划井可见性过滤器（setPlannedVisibilityFilter 注入，可空）。
    std::function<bool(const QString &id)> m_plannedVisible;
    void buildDataOpsUi();          // P3 增量 UI（过滤条/队列/视图页）
    void refreshAssetTree();
    // ---- 方向 92：树蓝图 + 增量通道（定义与实现见 datalist_tree.cpp）----
    struct NavNodeSpec;             // 树蓝图节点（纯数据，无 widget）
    QVector<NavNodeSpec> buildNavTreeSpec() const;
    void renderNavChildren(QTreeWidgetItem *parent, const QVector<NavNodeSpec> &specs);
    void rebuildNavTree(const QVector<NavNodeSpec> &specs);
    bool reconcileNavTree(const QVector<NavNodeSpec> &specs);
    void reconcileNavChildren(QTreeWidgetItem *parent, const QVector<NavNodeSpec> &specs,
                              bool apply, int *changed);
    static void applyNavSpec(QTreeWidgetItem *item, const NavNodeSpec &spec, bool newItem);
    static int navCountSpecNodes(const NavNodeSpec &spec);
    static bool navSpecDiffers(const QTreeWidgetItem *item, const NavNodeSpec &spec);
    TreeRefreshMode m_lastTreeRefreshMode = TreeRefreshMode::Unchanged;
    void renderAssetPage();
    StorageGovernanceController *m_storageController = nullptr;
    QPointer<StorageGovernanceDialog> m_storageDialog;
    QHash<QString, AssetEntityChoiceModel *> m_entityChoiceModels;
    QVector<paleo::dataops::AssetRowInfo> m_pageRows, m_filteredRows;
    QSet<QString> m_pageSelection;
    int m_assetPage = 0;
    int m_assetSortColumn = -1;
    bool m_tableDirty = true;
    bool m_assetSortAscending = true;
    void rebuildRowSnapshot();      // m_rows 装配（stores + catalog）
    void refreshSelectionBadge();
    void refreshTagCloud();
    void updatePendingCounts();
    void applyFilterToTree(const QSet<QString> &visibleIds, bool filtering);
    void registerCommands();        // D6.2 命令登记
    QTreeWidgetItem *treeItemForAsset(const QString &assetId) const;
    void refreshUndoButtons();      // D5.2 撤销/重做按钮文案与可用态

    // ---- 方向 30：体检/版本面辅助 ----
    QVector<paleo::dataops::VersionRow> versionRowsForAsset(const QString &assetId) const;
    CatalogHealthDialog *m_healthDlg = nullptr;
    paleo::health::HealthReport m_healthBase;
    int m_healthRecycleCount = 0;
    qint64 m_healthRecycleBytes = 0;
    void refreshHealthReportInDialog();
  public:
    void applyEntityDrop(const QStringList &assetIds, const QString &entityId); // D3.1/D3.4（拖放核心，批量挂接共用）
  private:
    void showAssetContextMenu(QObject *source, const QPoint &pos);              // D1.3
    void loadStoresForCatalog();   // catalog 会话变化 → stores 重载 + 栈清空
    // 旧搜索/类型控件 → FilterGroup 同步（兼容面：assetSearchEdit/assetTypeFilter
    // 仍是 Search/Type 维度的输入口）。
    void syncLegacyControlsIntoFilter();

    PreviewDocService *m_doc = nullptr;
    paleo::dataops::DataNavTree *m_tree = nullptr;
    QStackedWidget *m_viewStack = nullptr;
    // D5 视图命令栈（挂接/标签/改型/软删…；壳订阅 stackChanged 刷按钮态）。
    paleo::dataops::DataOpsUndoStack *m_opStack = nullptr;

    // ---- P3 dataops 状态 ----
    paleo::dataops::DataOpsContext m_ctx;              // catalog + stores 指针包
    paleo::dataops::TagStore m_tags;                   // D2.4
    paleo::dataops::AssetOverrideStore m_typeOv;       // D1.5
    paleo::dataops::EntityOverrideStore m_entityOv;    // D4.1/D4.2
    paleo::dataops::RecycleBin m_recycle;              // D1.6
    class WellAttachmentPanel *m_attachments = nullptr; // 方向 79（按需建）
    paleo::dataops::FilterGroup m_filter;              // D2
    QString m_activeTag;                               // D2.4 标签云激活
    bool m_treeWasFiltering = false;                   // 「清空搜索即收拢」只在过滤态退出那一跳执行
    QVector<paleo::dataops::AssetRowInfo> m_rows;      // 行快照（过滤输入）
    paleo::dataops::SelectionKeeper m_selKeep;         // D1.10
    paleo::dataops::TreeSortKind m_treeSort = paleo::dataops::TreeSortKind::Name; // D2.5
    std::shared_ptr<paleo::dataops::OperationsHistory> m_history; // D4.10
    paleo::dataops::CommandRegistry m_reg;             // D6.2
    // D1.4 批量挂接候选（EntityPickerDialog 结果暂存，供测试断言）。
    QString m_lastBatchTarget;
    // D7.6 分组维度。
    int m_groupMode = 0;
    // D2.7 高亮委托 / D1 程序化选中守卫 / catalog 会话记忆（D5.6）。
    paleo::dataops::HighlightDelegate *m_delegate = nullptr;
    bool m_progSelect = false;
    QString m_lastCatalogPath;
    // P3 部件指针（buildDataOpsUi 创建；objectName 见各部件）。
    QPushButton *m_undoBtn = nullptr;                       // dataUndoButton
    QPushButton *m_redoBtn = nullptr;                       // dataRedoButton
    paleo::dataops::FilterBar *m_filterBar = nullptr;
    paleo::dataops::FilterChipBar *m_chipBar = nullptr;
    paleo::dataops::PendingQuickBar *m_quickBar = nullptr;
    paleo::dataops::TagCloudWidget *m_tagCloud = nullptr;
    paleo::dataops::FilterEmptyState *m_emptyState = nullptr;
    paleo::dataops::AssetIconView *m_iconView = nullptr;
    paleo::dataops::AssetVirtualView *m_virtualView = nullptr;
    paleo::dataops::AssetGroupTree *m_groupTree = nullptr;
    paleo::dataops::ImportQueuePanel *m_importQueue = nullptr;
    QList<QPair<QString, QString>> m_shortcuts;             // (键串, objectName)
    void refreshChipBar();                                  // chip/词表/预设刷新
};
