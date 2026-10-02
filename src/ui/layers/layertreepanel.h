// 层：视图
#pragma once

#include <QList>
#include <QString>
#include <QWidget>

class QgsLayerTreeGroup;
class QgsLayerTreeLayer;
class QgsLayerTreeModel;
class QgsLayerTreeNode;
class QgsLayerTreeView;
class QgsMapCanvas;
class QgsMapLayer;
class QgsProject;
class QgisLayerService;
class QEvent;
class QLabel;
class QLineEdit;
class QAction;
class QMenu;

// 图层管理面板 —— 替换壳里裸 QgsLayerTreeView 的挂接（paleomainwindow 只
// new 这个面板）。objectName 兼容（tst_ui 依赖）：view="layerTreeView"、
// 空态 label="layerTreeEmptyState"（文本沿用现文案，QLabel 族）。
//
// 面板只渲染 + 发意图信号；显示/隐藏/排序/重命名/删除直接落在
// QgsLayerTreeModel/View（唯一图层状态源），面板不自建可见性台账。
//
// 结构：顶部工具条（添加组/删除选中/展开全部/折叠全部/筛选输入框）+
// QgsLayerTreeView（保留 AllowNodeReorder/Rename/ChangeVisibility 三 flag）
// + 右键菜单（QgsLayerTreeViewDefaultActions 全量组 + Paleo 自加项）+
// 空态 label（随工程图层集显隐）。
class LayerTreePanel : public QWidget
{
  Q_OBJECT

  public:
    LayerTreePanel(QgsProject *project, QgsMapCanvas *canvas,
                   QgisLayerService *layerService, QWidget *parent = nullptr);

    QgsLayerTreeView *treeView() const;
    QgsLayerTreeModel *layerTreeModel() const;

    // 工具条筛选框：按 layer title/name/id 大小写不敏感包含匹配；组内任一
    // 后代命中即保留该组；空串恢复全显。实现走 view->setRowHidden（不引
    // 代理——QgsLayerTreeView 强类型要求 QgsLayerTreeModel）。
    void setFilterText(const QString &text);
    QString filterText() const;

    // indicator 刷新：
    //  - 未实例化层位图层灰显 + tooltip「该图层属于层位 X（未激活）」；
    //  - 缺源图层（provider 读不到 → layer->isValid()==false）警示 indicator。
    void refreshIndicators();

  signals:
    // 「属性…」→ 壳接 LayerPropertiesDialog::openLayerProperties
    void propertiesRequested(const QString &layerId);
    // goal/gridding-surface-ops：栅格面运算（等厚/体积）入口——意图信号回壳。
    void surfaceOpsRequested(const QString &layerId);
    // 主线5「删除选中」消歧：编辑中的图层被从树里删时拒绝并带原因
    //（壳接状态栏/消息条展示；收尾会话走编辑工具条/属性表面板）。
    void layerRemovalRefused(const QString &reason);
    // 「在新页打开所属编图页」→ 壳接 showPage（组→页映射在面板内定义）
    void mappingPageRequested(const QString &pageId);
    // 「复制图层」直接落 QgsProject（克隆层 + 同组插入），不发信号。
    // 「导出/加载样式 .qml」经 QFileDialog + QgsMapLayer::export/importNamedStyle。

  private:
    // ---- 构建与刷新 ----
    QWidget *buildToolbar();
    void buildContextMenu();
    void updateEmptyState();
    void updatePaleoActionStates();
    void expandNewLayerNodes(const QList<QgsMapLayer *> &layers);
    // ---- 筛选（组内任一后代命中→组保留；空串全显） ----
    bool filterGroup(QgsLayerTreeGroup *group);      // 返回该组是否可见
    bool layerMatchesFilter(QgsLayerTreeLayer *node) const;
    void applyFilter();
    // ---- 右键 Paleo 项 ----
    QString currentLayerGroup() const;               // 树上组名，其次 decl.group
    void duplicateCurrentLayer();
    void exportCurrentStyle();
    void importCurrentStyle();
    // ---- indicator ----
    void scheduleIndicatorRefresh(); // 排队合并刷新（setActiveHorizon 时序）

    // ---- 空态 label 由 PaleoEmptyStateLabel 自持（含宿主 resize 居中） ----

    QgsProject *m_project = nullptr;
    QgsMapCanvas *m_canvas = nullptr;
    QgisLayerService *m_layerService = nullptr;
    QgsLayerTreeView *m_view = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QLabel *m_emptyState = nullptr; // PaleoEmptyStateLabel（共享空态组件）
    QMenu *m_menu = nullptr;
    QAction *m_addGroupAction = nullptr;       // 复用 defaultActions()
    QAction *m_removeAction = nullptr;         // 编辑守卫包装版（工具条+菜单共用）
    QAction *m_propertiesAction = nullptr;     // objectName: layerTreePropertiesAction
    QAction *m_duplicateAction = nullptr;      // objectName: layerTreeDuplicateAction
    QAction *m_surfaceOpsAction = nullptr;     // objectName: layerTreeSurfaceOpsAction（栅格面运算）
    QAction *m_exportStyleAction = nullptr;    // objectName: layerTreeExportStyleAction
    QAction *m_importStyleAction = nullptr;    // objectName: layerTreeImportStyleAction
    QAction *m_openPageAction = nullptr;       // objectName: layerTreeOpenMappingPageAction
    QString m_filterText;
    bool m_refreshQueued = false;
};
