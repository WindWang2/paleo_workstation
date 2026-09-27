// 层：视图
#pragma once

#include <QString>
#include <QWidget>

class QgsLayerTreeModel;
class QgsLayerTreeView;
class QgsMapCanvas;
class QgsProject;
class QgisLayerService;
class QLabel;
class QLineEdit;

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
    // 「在新页打开所属编图页」→ 壳接 showPage（组→页映射在面板内定义）
    void mappingPageRequested(const QString &pageId);
    // 「复制图层」直接落 QgsProject（克隆层 + 同组插入），不发信号。
    // 「导出/加载样式 .qml」经 QFileDialog + QgsMapLayer::export/importNamedStyle。

  private:
    QgsProject *m_project = nullptr;
    QgsMapCanvas *m_canvas = nullptr;
    QgisLayerService *m_layerService = nullptr;
    QgsLayerTreeView *m_view = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QLabel *m_emptyState = nullptr;
};
