// 层：视图
#include "layertreepanel.h"

#include "../paleoemptystate.h"
#include "../paleoicons.h"
#include "../paleotheme.h"
#include "../paleoviewport.h"

#include "../../qgis/layervocabulary.h"
#include "../../qgis/qgislayerorganizer.h"
#include "../../qgis/qgislayerservice.h"
#include "../../services/errorhub.h"
#include "../notifications/paleonotify.h"
#include "../shortcuts/shortcutcatalog.h"

#include <qgslayertree.h>
#include <qgslayertreelayer.h>
#include <qgslayertreemodel.h>
#include <qgslayertreeview.h>
#include <qgslayertreeviewdefaultactions.h>
#include <qgslayertreeviewindicator.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>
#include <qgsproject.h>

#include <QAction>
#include <QHash>
#include <QItemSelectionModel>
#include <QDomDocument>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QSaveFile>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
  // 组→编图页映射：词表单一权威在 qgis/layervocabulary.h（主线1）——
  // PaleoLayerVocabulary::pageForGroup 同时吸收旧组名（canonicalize 后再映射，
  // 旧 .qgz 声明的 "01_Prediction" 产层同样能跳 predict 页）。
  QString pageForGroup(const QString &group, QString *reason)
  {
    return PaleoLayerVocabulary::pageForGroup(group, reason);
  }

  // offscreen（测试/CI）无窗口系统：QFileDialog 一律跳过——硬纪律。
  bool noFileDialogs()
  {
    return QGuiApplication::platformName() == QLatin1String("offscreen");
  }
} // namespace

LayerTreePanel::LayerTreePanel(QgsProject *project, QgsMapCanvas *canvas,
                               QgisLayerService *layerService, QWidget *parent)
    : QWidget(parent), m_project(project), m_canvas(canvas), m_layerService(layerService)
{
  // DESIGN.md：面板 surface 底（token 活体注册，随主题重算）；正文 9pt
  //（pointSize 跟随系统缩放）。
  PaleoTheme::applyThemedStyleSheet(this, [] {
    return QStringLiteral("LayerTreePanel { background: %1; }")
        .arg(PaleoTheme::tokens().surface.name().toUpper());
  });
  QFont base = font();
  base.setPointSizeF(PaleoTheme::tokens().bodyPt);
  setFont(base);

  m_view = new QgsLayerTreeView(this);
  m_view->setObjectName(QStringLiteral("layerTreeView"));
  if (m_project && m_project->layerTreeRoot())
  {
    auto *model = new QgsLayerTreeModel(m_project->layerTreeRoot(), m_view);
    model->setFlag(QgsLayerTreeModel::AllowNodeReorder);
    model->setFlag(QgsLayerTreeModel::AllowNodeRename);
    model->setFlag(QgsLayerTreeModel::AllowNodeChangeVisibility);
    // 模型默认行为即显示图例（ShowLegend 默认开，ShowLegendAsTree 相关默认
    // 不动）；新增图层的 legend 展开由 expandNewLayerNodes 兜底。
    m_view->setModel(model);
    // 自定义绘制序生效时画布完全忽略树序——而拖拽 InternalMove 走 mime
    // 克隆（QgsLayerTreeModel::dropMimeData → insertChildNodes + 源
    // removeChildren），不经 moveCurrentNode；此处收集插入子树的图层，
    // 排队到事务落地（源节点移除完毕）后按终态树序回填 customLayerOrder。
    // 工具条挪组/跨组拖拽同被覆盖；「显示预测点位」特意置顶的条目因
    // slotIntoCustomOrder 的定点插入语义不被打平。
    connect(model, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &parent, int first, int last) {
              queueCustomOrderSync(parent, first, last);
            });
    // 勾选已有节点不触发 rowsInserted。旧工程或其它操作保留的自定义
    // 绘制序仍可能把图件压到不透明栅格之下；用户再次勾选时按树位补齐。
    connect(m_project->layerTreeRoot(), &QgsLayerTreeNode::visibilityChanged, this,
            [this](QgsLayerTreeNode *node) {
      if (!node || !node->isVisible()) return;
      auto *root = m_project->layerTreeRoot();
      if (auto *item = qobject_cast<QgsLayerTreeLayer *>(node)) {
        if (item->layer()) QgisLayerOrganizer::slotIntoCustomOrder(root, item->layer());
      } else if (auto *group = qobject_cast<QgsLayerTreeGroup *>(node)) {
        const auto children = group->findLayers();
        for (auto it = children.crbegin(); it != children.crend(); ++it)
          if ((*it)->isVisible() && (*it)->layer()) QgisLayerOrganizer::slotIntoCustomOrder(root, (*it)->layer());
      }
      if (m_canvas) m_canvas->refresh();
    });
  }

  // 空态卡片：共享组件 PaleoEmptyStateLabel（token 活体样式 + 宿主 resize
  // 自居中），objectName/文案钉死不变（tst_layertreepanel/tst_ui 断言）。
  m_emptyState = new PaleoEmptyStateLabel(
      tr("图层树是空的 — 导入数据后图层会出现在这里"), m_view);
  m_emptyState->setObjectName(QStringLiteral("layerTreeEmptyState"));
  m_emptyState->raise();

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(new PaleoToolRow(buildToolbar(), this));
  layout->addWidget(m_view, 1);

  buildContextMenu();
  connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
    updatePaleoActionStates(); // 弹出前按当前选中刷新 Paleo 项可用态
    m_menu->popup(m_view->viewport()->mapToGlobal(p));
  });
  m_view->setContextMenuPolicy(Qt::CustomContextMenu);
  if (m_view->selectionModel())
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            &LayerTreePanel::updatePaleoActionStates);

  // indicator 刷新触发面：构造时 + service 声明/实例化/释放 + 工程层增删。
  // service 三信号走排队刷新：setActiveHorizon 先发 horizonReleased 再改
  // m_activeHorizon，同步刷新会读到旧激活层位；排队到本轮调用栈之后必得
  // 终态（QgisLayerService 无 activeHorizonChanged 信号，不越权改它）。
  if (m_layerService)
  {
    connect(m_layerService, &QgisLayerService::layerDeclared, this,
            &LayerTreePanel::scheduleIndicatorRefresh);
    connect(m_layerService, &QgisLayerService::layerInstantiated, this,
            &LayerTreePanel::scheduleIndicatorRefresh);
    connect(m_layerService, &QgisLayerService::horizonReleased, this,
            &LayerTreePanel::scheduleIndicatorRefresh);
    // 实例化失败的声明层不进树——面板必须在树外把失败说出来，否则缺源
    // 文件在层位切换后「无声消失」。warning 级非模态通知卡（DESIGN.md
    // 错误呈现）；未安装全局 ErrorHub（单测/组装根之前）降级 stderr。
    connect(m_layerService, &QgisLayerService::horizonInstantiationFailed, this,
            [this](const QString &horizon, const QStringList &failures) {
              const QString title = tr("层位 %1 有 %n 个图层无法加载", nullptr,
                                       failures.size()).arg(horizon);
              if (ErrorHub *hub = ErrorHub::global())
                hub->raise(ErrorHub::Level::Warning, QStringLiteral("layers"),
                           title, failures.join(QLatin1Char('\n')));
              else
                qWarning("%s", qPrintable(QString(title + QStringLiteral(": ") +
                                                 failures.join(QLatin1Char(';')))));
            });
  }
  if (m_project)
  {
    // legendLayersAdded 在树节点落位之后发（layersAdded 先于节点落位）：
    // 空态/indicator/legend 展开都挂它；layersAdded/layersRemoved 兜住
    // addToLegend=false 的直加路径（复制图层）与删除路径。
    connect(m_project, &QgsProject::legendLayersAdded, this,
            [this](const QList<QgsMapLayer *> &layers) {
              expandNewLayerNodes(layers);
              updateEmptyState();
              refreshIndicators();
            });
    connect(m_project, &QgsProject::layersAdded, this,
            [this](const QList<QgsMapLayer *> &layers) {
              expandNewLayerNodes(layers);
              updateEmptyState();
              refreshIndicators();
            });
    connect(m_project, &QgsProject::layersRemoved, this,
            [this](const QStringList &) {
              updateEmptyState();
              refreshIndicators();
            });
  }

  updateEmptyState();
  refreshIndicators();
  updatePaleoActionStates();
}

QgsLayerTreeView *LayerTreePanel::treeView() const { return m_view; }

QgsLayerTreeModel *LayerTreePanel::layerTreeModel() const { return m_view->layerTreeModel(); }

// ---- 工具条 ----

QWidget *LayerTreePanel::buildToolbar()
{
  auto *bar = new QWidget(this);
  auto *lay = new QHBoxLayout(bar);
  lay->setContentsMargins(PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm, PaleoTheme::tokens().spacingSm); // DESIGN.md sm=8
  lay->setSpacing(PaleoTheme::tokens().spacingXs);                  // DESIGN.md xs=4（工具栏按钮间距）

  // 复用 QgsLayerTreeViewDefaultActions 的现成动作（工具条 + 右键菜单共用）。
  auto *acts = m_view->defaultActions();

  m_addGroupAction = acts->actionAddGroup(this);
  m_addGroupAction->setText(tr("添加组"));
  m_addGroupAction->setIcon(
      PaleoIcons::qgisTheme(QStringLiteral("mActionAddGroup.svg")));

  auto *removeAction = acts->actionRemoveGroupOrLayer(this);
  // 主线5 消歧：明确动作对象是「图层树的图层/组」，不是画布上选中的要素
  //（要素删除在编辑工具条）。默认动作（右键菜单共用实例）同步改口。
  removeAction->setText(tr("删除所选图层/组"));
  removeAction->setToolTip(
      tr("删除图层树里选中的图层或组；画布上选中的要素用编辑工具条的「删除」"));

  // 删除走守卫包装（工具条按钮与右键菜单共用同一 QAction 实例——C6）：
  // 编辑会话中的图层先收尾（保存/放弃）再删——直接删会把未提交的编辑随
  // 图层析构静默丢弃。
  m_removeAction = new QAction(
      PaleoIcons::qgisTheme(QStringLiteral("mActionRemoveLayer.svg")),
      tr("删除所选图层/组"), this);
  m_removeAction->setObjectName(QStringLiteral("layerTreeRemoveSelectedAction"));
  m_removeAction->setToolTip(removeAction->toolTip());
  // goal/ui-experience-polish：Delete 键直达删除（桌面列表惯例；QAction 挂
  // 键后菜单/工具条按钮同步显示快捷键提示）。
  paleo::shortcuts::bindAction(QStringLiteral("layers.remove"), m_removeAction); // 方向63 登记
  addAction(m_removeAction);
  connect(m_removeAction, &QAction::triggered, this, [this, removeAction] {
    QList<QgsMapLayer *> selected =
        m_view ? m_view->selectedLayers() : QList<QgsMapLayer *>();
    if (selected.isEmpty() && m_view && m_view->currentLayer())
      selected = { m_view->currentLayer() }; // setCurrentLayer 未建立选区的路径
    for (QgsMapLayer *l : selected)
    {
      if (auto *vl = qobject_cast<QgsVectorLayer *>(l); vl && vl->isEditable())
      {
        emit layerRemovalRefused(
            tr("图层「%1」正在编辑——先保存或放弃编辑，再从图层树删除").arg(vl->name()));
        return;
      }
    }
    removeAction->trigger();
  });

  // 图标化工具条：文字进 tooltip/accessibleName，按钮只显示图标（与连井剖面工具条同款）。
  auto mkButton = [bar](const QString &objectName, QAction *action) {
    auto *btn = new QToolButton(bar);
    btn->setObjectName(objectName);
    btn->setDefaultAction(action);
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    btn->setIconSize(PaleoIcons::toolbarSize());
    btn->setAutoRaise(true);
    btn->setToolTip(action->text());
    btn->setAccessibleName(action->text());
    return btn;
  };
  lay->addWidget(mkButton(QStringLiteral("layerTreeAddGroupButton"), m_addGroupAction));
  lay->addWidget(mkButton(QStringLiteral("layerTreeRemoveSelectedButton"), m_removeAction));

  // 列表序即绘制序：序号越小越靠上，也越后画（盖住下面的图层）。
  m_moveUpAction = new QAction(
      PaleoIcons::qgisTheme(QStringLiteral("mActionArrowUp.svg")), tr("上移"), this);
  m_moveUpAction->setObjectName(QStringLiteral("layerTreeMoveUpAction"));
  connect(m_moveUpAction, &QAction::triggered, this, [this]() {
    const int from = currentNodeIndex();
    if (from > 0)
      moveCurrentNode(from - 1);
  });
  m_moveDownAction = new QAction(
      PaleoIcons::qgisTheme(QStringLiteral("mActionArrowDown.svg")), tr("下移"), this);
  m_moveDownAction->setObjectName(QStringLiteral("layerTreeMoveDownAction"));
  connect(m_moveDownAction, &QAction::triggered, this, [this]() {
    int count = 0;
    const int from = currentNodeIndex(&count);
    if (from >= 0 && from + 1 < count)
      moveCurrentNode(from + 1);
  });
  m_moveTopAction = new QAction(tr("置顶（最上层）"), this);
  m_moveTopAction->setObjectName(QStringLiteral("layerTreeMoveToTopAction"));
  connect(m_moveTopAction, &QAction::triggered, this, [this]() {
    if (currentNodeIndex() > 0)
      moveCurrentNode(0);
  });
  lay->addWidget(mkButton(QStringLiteral("layerTreeMoveUpButton"), m_moveUpAction));
  lay->addWidget(mkButton(QStringLiteral("layerTreeMoveDownButton"), m_moveDownAction));

  auto *expandAct = new QAction(
      PaleoIcons::qgisTheme(QStringLiteral("mActionExpandTree.svg")),
      tr("展开全部"), this);
  connect(expandAct, &QAction::triggered, m_view, &QgsLayerTreeView::expandAllNodes);
  lay->addWidget(mkButton(QStringLiteral("layerTreeExpandAllButton"), expandAct));
  auto *collapseAct = new QAction(
      PaleoIcons::qgisTheme(QStringLiteral("mActionCollapseTree.svg")),
      tr("折叠全部"), this);
  connect(collapseAct, &QAction::triggered, m_view, &QgsLayerTreeView::collapseAllNodes);
  lay->addWidget(mkButton(QStringLiteral("layerTreeCollapseAllButton"), collapseAct));

  m_filterEdit = new QLineEdit(bar);
  m_filterEdit->setObjectName(QStringLiteral("layerTreeFilterEdit"));
  m_filterEdit->setPlaceholderText(tr("筛选图层…"));
  m_filterEdit->setClearButtonEnabled(true);
  connect(m_filterEdit, &QLineEdit::textChanged, this, &LayerTreePanel::setFilterText);
  lay->addWidget(m_filterEdit, 1);
  return bar;
}

// ---- 右键菜单 ----

void LayerTreePanel::buildContextMenu()
{
  m_menu = new QMenu(m_view);
  auto *acts = m_view->defaultActions();

  // QGIS 默认组：缩放两项在 canvas 为 null 时隐藏（照壳内守卫写法）。
  if (m_canvas)
  {
    m_menu->addAction(acts->actionZoomToLayers(m_canvas, m_menu));
    m_menu->addAction(acts->actionZoomToSelection(m_canvas, m_menu));
  }
  m_menu->addAction(acts->actionShowFeatureCount(m_menu));
  m_menu->addSeparator();
  m_menu->addAction(m_moveUpAction);
  m_menu->addAction(m_moveDownAction);
  m_menu->addAction(m_moveTopAction);
  m_menu->addSeparator();
  m_menu->addAction(acts->actionRenameGroupOrLayer(m_menu));
  // C6：右键菜单与工具条共用同一守卫版删除动作——编辑中的图层在任何
  // 入口都必须先收尾（守卫内 emit layerRemovalRefused 给原因）。
  m_menu->addAction(m_removeAction);
  m_menu->addAction(m_addGroupAction);
  m_menu->addSeparator();

  // Paleo 组：属性/复制/样式/编图页——意图信号回壳。
  m_propertiesAction = new QAction(tr("属性…"), m_menu);
  m_propertiesAction->setObjectName(QStringLiteral("layerTreePropertiesAction"));
  connect(m_propertiesAction, &QAction::triggered, this, [this]() {
    QgsMapLayer *layer = m_view->layerTreeModel() ? m_view->currentLayer() : nullptr;
    if (!layer)
      return;
    // manifest 管辖层用 paleoLayerId；手工层回退 QgsMapLayer::id()（自动生成串）。
    const QString paleoId =
        layer->customProperty(QStringLiteral("paleoLayerId")).toString();
    emit propertiesRequested(paleoId.isEmpty() ? layer->id() : paleoId);
  });
  m_menu->addAction(m_propertiesAction);

  m_attributeTableAction = new QAction(tr("属性表"), m_menu);
  m_attributeTableAction->setObjectName(QStringLiteral("layerTreeAttributeTableAction"));
  connect(m_attributeTableAction, &QAction::triggered, this, [this]() {
    QgsMapLayer *layer = m_view->layerTreeModel() ? m_view->currentLayer() : nullptr;
    auto *vector = qobject_cast<QgsVectorLayer *>(layer);
    if (!vector)
      return;
    const QString paleoId =
        vector->customProperty(QStringLiteral("paleoLayerId")).toString();
    emit attributeTableRequested(paleoId.isEmpty() ? vector->id() : paleoId);
  });
  m_menu->addAction(m_attributeTableAction);

  m_graphAction = m_menu->addAction(tr("资料关联 / 成果追溯"));
  m_graphAction->setObjectName(QStringLiteral("layerTreeDataGraphAction"));
  m_graphAction->setToolTip(tr("查看该图层登记的资料版本、输入文件与生成证据"));
  connect(m_menu, &QMenu::aboutToShow, this, [this] {
    m_graphAction->setProperty("graphTarget", m_graphAction->data());
  });
  connect(m_graphAction, &QAction::triggered, this, [this] {
    const auto id = m_graphAction->property("graphTarget").toString();
    if (!id.isEmpty()) emit graphRequested(id);
  });

  // goal/gridding-surface-ops：栅格图层 → 面运算（等厚/体积）入口。
  // 动作常驻、显隐由 updatePaleoActionStates() 按右键当下当前层刷新；
  // 触发时再取 currentLayer()——构造期捕获裸层指针会悬垂/点错层。
  m_surfaceOpsAction = new QAction(tr("面运算（等厚/体积）…"), m_menu);
  m_surfaceOpsAction->setObjectName(QStringLiteral("layerTreeSurfaceOpsAction"));
  connect(m_surfaceOpsAction, &QAction::triggered, this, [this]() {
    QgsMapLayer *cur = m_view->layerTreeModel() ? m_view->currentLayer() : nullptr;
    if (!cur || cur->type() != Qgis::LayerType::Raster)
      return;
    const QString paleoId = cur->customProperty(QStringLiteral("paleoLayerId")).toString();
    emit surfaceOpsRequested(paleoId.isEmpty() ? cur->id() : paleoId);
  });
  m_menu->addAction(m_surfaceOpsAction);

  m_duplicateAction = new QAction(tr("复制图层"), m_menu);
  m_duplicateAction->setObjectName(QStringLiteral("layerTreeDuplicateAction"));
  connect(m_duplicateAction, &QAction::triggered, this,
          &LayerTreePanel::duplicateCurrentLayer);
  m_menu->addAction(m_duplicateAction);

  m_exportStyleAction = new QAction(tr("导出样式 .qml…"), m_menu);
  m_exportStyleAction->setObjectName(QStringLiteral("layerTreeExportStyleAction"));
  connect(m_exportStyleAction, &QAction::triggered, this,
          &LayerTreePanel::exportCurrentStyle);
  m_menu->addAction(m_exportStyleAction);

  m_importStyleAction = new QAction(tr("加载样式 .qml…"), m_menu);
  m_importStyleAction->setObjectName(QStringLiteral("layerTreeImportStyleAction"));
  connect(m_importStyleAction, &QAction::triggered, this,
          &LayerTreePanel::importCurrentStyle);
  m_menu->addAction(m_importStyleAction);

  m_openPageAction = new QAction(tr("在新页打开所属编图页"), m_menu);
  m_openPageAction->setObjectName(QStringLiteral("layerTreeOpenMappingPageAction"));
  connect(m_openPageAction, &QAction::triggered, this, [this]() {
    QString reason;
    const QString page = pageForGroup(currentLayerGroup(), &reason);
    if (!page.isEmpty())
      emit mappingPageRequested(page);
  });
  m_menu->addAction(m_openPageAction);

  // goal/time-depth-velocity：时间域层位 → 深度域（意图信号，编排在壳外）。
  m_depthConvertAction = new QAction(tr("转换为深度域…"), m_menu);
  m_depthConvertAction->setObjectName(QStringLiteral("layerTreeDepthConvertAction"));
  connect(m_depthConvertAction, &QAction::triggered, this, [this]() {
    QgsMapLayer *layer = m_view->layerTreeModel() ? m_view->currentLayer() : nullptr;
    if (!layer)
      return;
    const QString paleoId =
        layer->customProperty(QStringLiteral("paleoLayerId")).toString();
    if (!paleoId.startsWith(QStringLiteral("horizon.")))
      return; // 仅时间域层位（守卫与可用态同口径）
    emit depthConversionRequested(paleoId);
  });
  m_menu->addAction(m_depthConvertAction);
}

int LayerTreePanel::currentNodeIndex(int *siblingCount) const
{
  if (siblingCount)
    *siblingCount = 0;
  if (!m_view)
    return -1;
  QgsLayerTreeNode *node = m_view->currentNode();
  auto *parent = node ? qobject_cast<QgsLayerTreeGroup *>(node->parent()) : nullptr;
  if (!parent)
    return -1;
  const QList<QgsLayerTreeNode *> kids = parent->children();
  if (siblingCount)
    *siblingCount = kids.size();
  return kids.indexOf(node);
}

void LayerTreePanel::moveCurrentNode(int toIndex)
{
  if (!m_view)
    return;
  QgsLayerTreeNode *node = m_view->currentNode();
  auto *parent = node ? qobject_cast<QgsLayerTreeGroup *>(node->parent()) : nullptr;
  if (!parent)
    return;
  const QList<QgsLayerTreeNode *> kids = parent->children();
  const int from = kids.indexOf(node);
  if (from < 0 || toIndex < 0 || toIndex >= kids.size() || toIndex == from)
    return;
  // insertChildNode 接管无父节点。先插入副本再摘掉原节点，避免 remove 把节点删掉。
  QgsLayerTreeNode *placed = node->clone();
  parent->insertChildNode(toIndex > from ? toIndex + 1 : toIndex, placed);
  parent->removeChildNode(node);
  m_view->setCurrentNode(placed);
  // 自定义绘制序生效时画布完全忽略树序——树里挪了层但自定义序不同步，
  // 画布叠置就停在旧位（opt-05）。只把被挪子树的图层按新树序插回序列表
  //（slotIntoCustomOrder 的既有语义：置于树序中其下首个已入序图层之前），
  // 其它条目原位保留——「显示预测点位」特意置顶的非树序条目不被打平。
  // 挪的是组时组内图层逐个回填；自下而上处理，上层成员锚定到已归位的
  // 下层条目而非其旧位。
  if (QgsLayerTree *root = m_view->layerTreeModel()
                               ? m_view->layerTreeModel()->rootGroup()
                               : nullptr)
  {
    QList<QgsLayerTreeLayer *> moved;
    if (auto *ln = qobject_cast<QgsLayerTreeLayer *>(placed))
      moved << ln;
    else if (auto *g = qobject_cast<QgsLayerTreeGroup *>(placed))
      moved = g->findLayers(); // 递归：树序
    for (int i = moved.size() - 1; i >= 0; --i)
      if (QgsMapLayer *ml = moved.at(i)->layer())
        QgisLayerOrganizer::slotIntoCustomOrder(root, ml);
  }
  if (QItemSelectionModel *sel = m_view->selectionModel())
  {
    const QModelIndex idx = m_view->layerTreeModel()->node2index(placed);
    if (idx.isValid())
      sel->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  }
  refreshIndicators();
  updatePaleoActionStates();
}

void LayerTreePanel::queueCustomOrderSync(const QModelIndex &parent, int first,
                                          int last)
{
  QgsLayerTreeModel *model = m_view ? m_view->layerTreeModel() : nullptr;
  QgsLayerTree *root = model ? model->rootGroup() : nullptr;
  if (!root || !root->hasCustomLayerOrder())
    return; // 未启用自定义序：画布本就跟随树序，收集是纯开销
  for (int r = first; r <= last; ++r)
  {
    QgsLayerTreeNode *n = model->index2node(model->index(r, 0, parent));
    if (auto *ln = qobject_cast<QgsLayerTreeLayer *>(n))
    {
      if (ln->layer())
        m_pendingOrderLayers.insert(ln->layer());
    }
    else if (auto *g = qobject_cast<QgsLayerTreeGroup *>(n))
      for (QgsLayerTreeLayer *child : g->findLayers())
        if (child->layer())
          m_pendingOrderLayers.insert(child->layer());
  }
  if (!m_pendingOrderLayers.isEmpty() && !m_orderSyncQueued)
  {
    m_orderSyncQueued = true;
    QMetaObject::invokeMethod(this, &LayerTreePanel::flushCustomOrderSync,
                              Qt::QueuedConnection);
  }
}

void LayerTreePanel::flushCustomOrderSync()
{
  m_orderSyncQueued = false;
  const QSet<QgsMapLayer *> pending = std::move(m_pendingOrderLayers);
  QgsLayerTreeModel *model = m_view ? m_view->layerTreeModel() : nullptr;
  QgsLayerTree *root = model ? model->rootGroup() : nullptr;
  if (!root || pending.isEmpty())
    return;
  // 依终态树序自下而上回填：上层被挪层锚定到已归位的下层条目。set 里
  // 可能有已离树图层（拖入后又被删）——slotIntoCustomOrder 找不到其树
  // 位时幂等返回。QSet 成员皆为指针比较，不触碰悬垂对象本体。
  const QList<QgsLayerTreeLayer *> treeLayers = root->findLayers();
  for (int i = treeLayers.size() - 1; i >= 0; --i)
    if (pending.contains(treeLayers.at(i)->layer()))
      QgisLayerOrganizer::slotIntoCustomOrder(root, treeLayers.at(i)->layer());
}

void LayerTreePanel::updatePaleoActionStates()
{
  QgsMapLayer *layer = m_view->layerTreeModel() ? m_view->currentLayer() : nullptr;
  int siblingCount = 0;
  const int nodeIndex = currentNodeIndex(&siblingCount);
  const bool hasNode = nodeIndex >= 0;
  if (m_moveUpAction && m_moveDownAction && m_moveTopAction)
  {
    const bool canUp = nodeIndex > 0;
    const bool canDown = hasNode && nodeIndex + 1 < siblingCount;
    m_moveUpAction->setEnabled(canUp);
    m_moveDownAction->setEnabled(canDown);
    m_moveTopAction->setEnabled(canUp);
    m_moveUpAction->setToolTip(
        canUp ? tr("在所在组内上移一层。列表越靠上，绘制越靠上")
              : (hasNode ? tr("已在所在组的最上层") : tr("未选中图层或组")));
    m_moveDownAction->setToolTip(
        canDown ? tr("在所在组内下移一层。列表越靠下，绘制越靠下")
                : (hasNode ? tr("已在所在组的最下层") : tr("未选中图层或组")));
    m_moveTopAction->setToolTip(
        canUp ? tr("移到所在组的最上层。根上的图层即整个列表最上，盖住同组其他图层")
              : (hasNode ? tr("已在所在组的最上层") : tr("未选中图层或组")));
  }
  const QString graphId = layer ? layer->customProperty(QStringLiteral("paleoLayerId")).toString() : QString();
  m_graphAction->setEnabled(!graphId.isEmpty()); m_graphAction->setData(graphId);
  m_propertiesAction->setEnabled(layer != nullptr);
  const bool vectorLayer = qobject_cast<QgsVectorLayer *>(layer) != nullptr;
  m_attributeTableAction->setEnabled(vectorLayer);
  m_attributeTableAction->setToolTip(
      vectorLayer ? tr("在底部属性表面板打开该图层的要素表")
                  : (layer ? tr("仅矢量图层可打开属性表") : tr("未选中图层")));
  // 面运算只对当前栅格层可见（右键当下即时判定，不用构造期捕获）
  m_surfaceOpsAction->setVisible(layer && layer->type() == Qgis::LayerType::Raster);
  m_duplicateAction->setEnabled(layer != nullptr);
  m_exportStyleAction->setEnabled(layer != nullptr);
  m_importStyleAction->setEnabled(layer != nullptr);

  // 深度域转换只对时间域层位层开（paleoLayerId "horizon.*"；手工层/深度
  // 产物/矢量层不给入口——DESIGN.md 禁用带 reason）。
  const QString paleoId =
      layer ? layer->customProperty(QStringLiteral("paleoLayerId")).toString() : QString();
  const bool convertible = paleoId.startsWith(QStringLiteral("horizon."));
  m_depthConvertAction->setEnabled(convertible);
  m_depthConvertAction->setToolTip(
      convertible ? QString()
                  : tr("仅时间域层位栅格可转换为深度域（先导入井分层/校验炮）"));

  if (!layer)
  {
    m_openPageAction->setEnabled(false);
    m_openPageAction->setToolTip(tr("未选中图层"));
    return;
  }
  QString reason;
  const QString page = pageForGroup(currentLayerGroup(), &reason);
  if (page.isEmpty())
  {
    m_openPageAction->setEnabled(false);
    m_openPageAction->setToolTip(reason); // DESIGN.md：禁用必须带 reason
  }
  else
  {
    m_openPageAction->setEnabled(true);
    m_openPageAction->setToolTip(tr("切换到该图层所属的编图页"));
  }
}

QString LayerTreePanel::currentLayerGroup() const
{
  QgsLayerTreeModel *model = m_view->layerTreeModel();
  if (!model)
    return QString();
  QgsMapLayer *layer = m_view->currentLayer();
  if (!layer)
    return QString();
  // 优先树上所属组节点名；树根直挂层再看 decl.group。
  // 例外：布局器建的层位组（paleoHorizon 属性）不是编图组——层位组内
  // 图层的所属页仍由 decl.group 决定。
  if (QgsLayerTreeLayer *node = model->rootGroup()->findLayer(layer->id()))
  {
    QgsLayerTreeNode *parent = node->parent();
    if (parent && parent != model->rootGroup() && QgsLayerTree::isGroup(parent)
        && !parent->customProperty(QStringLiteral("paleoHorizon")).isValid())
      return parent->name();
  }
  const QString paleoId =
      layer->customProperty(QStringLiteral("paleoLayerId")).toString();
  if (!paleoId.isEmpty() && m_layerService)
  {
    for (const LayerDeclaration &d : m_layerService->declared())
      if (d.layerId == paleoId)
        return d.group;
  }
  return QString();
}

// ---- 复制图层 ----

void LayerTreePanel::duplicateCurrentLayer()
{
  QgsLayerTreeModel *model = m_view->layerTreeModel();
  QgsMapLayer *layer = model ? m_view->currentLayer() : nullptr;
  if (!layer || !m_project)
    return;

  QgsMapLayer *dup = layer->clone();
  if (!dup)
    return;
  dup->setName(layer->name() + QStringLiteral(" 副本"));
  // 副本不再是 manifest 管辖层：清空 paleoLayerId 自定义属性，
  // 防 decl 回查（属性/indicator/组映射）把副本当原层双匹配。
  dup->removeCustomProperty(QStringLiteral("paleoLayerId"));

  // 不进 legend（addToLegend=false）：手动建节点插回原层同一父组（树根则根）。
  m_project->addMapLayer(dup, false);
  QgsLayerTree *root = model->rootGroup();
  QgsLayerTreeGroup *parent = root;
  if (QgsLayerTreeLayer *origNode = root->findLayer(layer->id()))
    if (QgsLayerTreeNode *p = origNode->parent())
      if (QgsLayerTree::isGroup(p))
        parent = QgsLayerTree::toGroup(p);
  parent->insertChildNode(-1, new QgsLayerTreeLayer(dup));
  m_view->setCurrentLayer(dup);
  // addToLegend=false 直加不触发 legendLayersAdded——手动补一次空态/indicator。
  updateEmptyState();
  refreshIndicators();
}

// ---- 导出/加载样式 ----

bool LayerTreePanel::exportStyleTo(QgsMapLayer *layer, const QString &path, QString *error)
{
  if (!layer || path.isEmpty())
  {
    if (error)
      *error = tr("没有可导出的图层。");
    return false;
  }
  QDomDocument doc;
  QString styleErr;
  layer->exportNamedStyle(doc, styleErr);
  if (!styleErr.isEmpty())
  {
    if (error)
      *error = styleErr;
    return false;
  }
  // 原子写：临时文件落盘后 commit 才替换目标——导出失败（含写入失败）
  // 不会把空/半成品文档烙进用户选定文件（B.17：旧代码 QFile Truncate
  // 直写，err 与 write() 返回值全部丢弃）。
  QSaveFile f(path);
  const QByteArray bytes = doc.toByteArray(2);
  if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
  {
    if (error)
      *error = f.errorString();
    return false;
  }
  return true;
}

bool LayerTreePanel::importStyleFrom(QgsMapLayer *layer, const QString &path, QString *error)
{
  if (!layer || path.isEmpty())
  {
    if (error)
      *error = tr("没有可导入样式的图层。");
    return false;
  }
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
  {
    if (error)
      *error = tr("无法读取文件：%1").arg(path);
    return false;
  }
  QDomDocument doc;
  if (!doc.setContent(&f))
  {
    if (error)
      *error = tr("不是有效的样式文件。");
    return false;
  }
  QString styleErr;
  if (!layer->importNamedStyle(doc, styleErr) || !styleErr.isEmpty())
  {
    if (error)
      *error = styleErr.isEmpty() ? tr("样式导入失败。") : styleErr;
    return false;
  }
  return true;
}

void LayerTreePanel::exportCurrentStyle()
{
  QgsMapLayer *layer = m_view->layerTreeModel() ? m_view->currentLayer() : nullptr;
  if (!layer || noFileDialogs())
    return;
  const QString path = QFileDialog::getSaveFileName(
      this, tr("导出样式"),
      layer->name() + QStringLiteral(".qml"), tr("QGIS 样式文件 (*.qml)"));
  if (path.isEmpty())
    return;
  QString err;
  if (!exportStyleTo(layer, path, &err))
    PaleoNotify::warning(this, tr("导出样式"), err);
}

void LayerTreePanel::importCurrentStyle()
{
  QgsMapLayer *layer = m_view->layerTreeModel() ? m_view->currentLayer() : nullptr;
  if (!layer || noFileDialogs())
    return;
  const QString path = QFileDialog::getOpenFileName(
      this, tr("加载样式"), QString(), tr("QGIS 样式文件 (*.qml)"));
  if (path.isEmpty())
    return;
  QString err;
  if (!importStyleFrom(layer, path, &err))
    PaleoNotify::warning(this, tr("加载样式"), err);
}

// ---- 筛选 ----

void LayerTreePanel::setFilterText(const QString &text)
{
  m_filterText = text;
  if (m_filterEdit && m_filterEdit->text() != text)
    m_filterEdit->setText(text); // 程序入口与输入框双向一致
  applyFilter();
}

QString LayerTreePanel::filterText() const { return m_filterText; }

void LayerTreePanel::applyFilter()
{
  QgsLayerTreeModel *model = m_view->layerTreeModel();
  if (model)
    filterGroup(model->rootGroup());
}

bool LayerTreePanel::filterGroup(QgsLayerTreeGroup *group)
{
  QgsLayerTreeModel *model = m_view->layerTreeModel();
  if (!group || !model)
    return true;
  const bool noFilter = m_filterText.isEmpty();
  bool anyVisible = false;
  const QList<QgsLayerTreeNode *> children = group->children();
  for (QgsLayerTreeNode *child : children)
  {
    bool visible = true;
    if (QgsLayerTree::isGroup(child))
    {
      // 始终递归进组（空串时也要把此前隐藏的后代行恢复全显）
      visible = filterGroup(QgsLayerTree::toGroup(child));
      if (noFilter)
        visible = true;
    }
    else if (!noFilter)
    {
      // 组内任一后代命中→组保留；非层非组节点无匹配语义
      visible = QgsLayerTree::isLayer(child)
                    ? layerMatchesFilter(QgsLayerTree::toLayer(child))
                    : false;
    }
    // view 内部有代理模型：node2index 必须走 view（proxy 感知），不能用
    // source model 的同名方法——索引模型对不上会被 setRowHidden 拒收。
    const QModelIndex idx = m_view->node2index(child);
    if (idx.isValid())
      m_view->setRowHidden(idx.row(), idx.parent(), !visible);
    anyVisible = anyVisible || visible;
  }
  return noFilter ? true : anyVisible;
}

bool LayerTreePanel::layerMatchesFilter(QgsLayerTreeLayer *node) const
{
  if (m_filterText.isEmpty())
    return true;
  QgsMapLayer *layer = node->layer();
  const QStringList candidates{
      node->name(), // 树上显示名（= layer title/name）
      layer ? layer->id() : QString(),
      layer ? layer->customProperty(QStringLiteral("paleoLayerId")).toString()
            : QString()};
  for (const QString &c : candidates)
    if (!c.isEmpty() && c.contains(m_filterText, Qt::CaseInsensitive))
      return true;
  return false;
}

// ---- indicator ----

void LayerTreePanel::scheduleIndicatorRefresh()
{
  if (m_refreshQueued)
    return; // 合并同轮多信号（setActiveHorizon 连发 release+instantiate）
  m_refreshQueued = true;
  QMetaObject::invokeMethod(
      this,
      [this]() {
        m_refreshQueued = false;
        refreshIndicators();
      },
      Qt::QueuedConnection);
}

void LayerTreePanel::refreshIndicators()
{
  QgsLayerTreeModel *model = m_view->layerTreeModel();
  if (!model)
    return;

  const QList<QgsLayerTreeLayer *> layerNodes = model->rootGroup()->findLayers();

  // 全量重建：先清后加。
  for (QgsLayerTreeLayer *node : layerNodes)
  {
    const QList<QgsLayerTreeViewIndicator *> inds = m_view->indicators(node);
    for (QgsLayerTreeViewIndicator *ind : inds)
    {
      m_view->removeIndicator(node, ind);
      ind->deleteLater();
    }
  }

  // 灰显回查需要 decl；layerService 可为 nullptr（全 guard）。
  const QString activeHorizon =
      m_layerService ? m_layerService->activeHorizon() : QString();
  // declared() 每次调用都是一次 sqlite 全表读——逐图层节点复读会把刷新
  // 放大成 O(层数×声明数) 次查询（goal7 W2 探针实测）。一次取表建哈希；
  // 同 layerId 取首个声明，与旧线性扫描的 break 语义一致。
  QHash<QString, QString> horizonById;
  if (m_layerService)
  {
    const QVector<LayerDeclaration> decls = m_layerService->declared();
    horizonById.reserve(decls.size());
    for (const LayerDeclaration &d : decls)
      if (!horizonById.contains(d.layerId))
        horizonById.insert(d.layerId, d.horizon);
  }

  for (QgsLayerTreeLayer *node : layerNodes)
  {
    QgsMapLayer *layer = node->layer();
    if (!layer)
      continue;
    if (!layer->isValid())
    {
      auto *ind = new QgsLayerTreeViewIndicator(m_view);
      ind->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mIconWarning.svg")));
      ind->setToolTip(tr("图层源不可用"));
      m_view->addIndicator(node, ind);
    }
    const QString paleoId =
        layer->customProperty(QStringLiteral("paleoLayerId")).toString();
    if (m_layerService && !paleoId.isEmpty())
    {
      const auto it = horizonById.constFind(paleoId);
      if (it != horizonById.constEnd() && !it.value().isEmpty()
          && it.value() != activeHorizon)
      {
        auto *ind = new QgsLayerTreeViewIndicator(m_view);
        ind->setIcon(PaleoIcons::inactive());
        ind->setToolTip(tr("该图层属于层位 %1（未激活）").arg(it.value()));
        m_view->addIndicator(node, ind);
      }
    }
  }
}

// ---- 空态 ----

void LayerTreePanel::updateEmptyState()
{
  const bool empty = m_project && m_project->mapLayers().isEmpty();
  m_emptyState->setHidden(!empty);
}

void LayerTreePanel::expandNewLayerNodes(const QList<QgsMapLayer *> &layers)
{
  QgsLayerTreeModel *model = m_view->layerTreeModel();
  if (!model)
    return;
  for (QgsMapLayer *l : layers)
  {
    if (!l)
      continue;
    if (QgsLayerTreeNode *node = model->rootGroup()->findLayer(l->id()))
      node->setExpanded(true); // 新增图层 legend 符号节点展开
  }
}
