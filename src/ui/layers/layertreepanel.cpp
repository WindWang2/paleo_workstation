// 层：视图
#include "layertreepanel.h"

#include "../../qgis/layervocabulary.h"
#include "../../qgis/qgislayerservice.h"

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

  // 灰显 indicator 图标：text-disabled 灰（DESIGN.md #9AA7B4）实心圆点——
  // 「未激活」是状态语义，不占交互蓝，不属装饰色约束。
  QIcon greyDotIcon()
  {
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(QStringLiteral("#9AA7B4")));
    p.drawEllipse(2, 2, 8, 8);
    return QIcon(pm);
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
  // DESIGN.md：面板 surface #FFFFFF 白底；正文 9pt（pointSize 跟随系统缩放）。
  setStyleSheet(QStringLiteral("LayerTreePanel { background: #FFFFFF; }"));
  QFont base = font();
  base.setPointSizeF(9.0);
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
  }

  // 空态 label：壳里 EmptyStateLabel 的等价物（那类是 mainwindow 私有，
  // 这里自带）——白底半透明卡片、居中、随宿主 resize 保持居中。
  m_emptyState = new QLabel(
      QStringLiteral("图层树是空的 — 导入数据后图层会出现在这里"), m_view);
  m_emptyState->setObjectName(QStringLiteral("layerTreeEmptyState"));
  m_emptyState->setAlignment(Qt::AlignCenter);
  m_emptyState->setWordWrap(true);
  m_emptyState->setStyleSheet(QStringLiteral(
      "background: rgba(255,255,255,0.9); color: #5D6E80; padding: 12px 16px;"
      "border: 1px solid #DFE5EC; border-radius: 8px;"));
  m_view->installEventFilter(this);
  m_emptyState->raise();

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(buildToolbar());
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
  recenterEmptyState();
}

QgsLayerTreeView *LayerTreePanel::treeView() const { return m_view; }

QgsLayerTreeModel *LayerTreePanel::layerTreeModel() const { return m_view->layerTreeModel(); }

// ---- 工具条 ----

QWidget *LayerTreePanel::buildToolbar()
{
  auto *bar = new QWidget(this);
  auto *lay = new QHBoxLayout(bar);
  lay->setContentsMargins(8, 8, 8, 8); // DESIGN.md sm=8
  lay->setSpacing(4);                  // DESIGN.md xs=4（工具栏按钮间距）

  // 复用 QgsLayerTreeViewDefaultActions 的现成动作（工具条 + 右键菜单共用）。
  auto *acts = m_view->defaultActions();

  m_addGroupAction = acts->actionAddGroup(this);
  m_addGroupAction->setText(tr("添加组"));

  auto *removeAction = acts->actionRemoveGroupOrLayer(this);
  // 主线5 消歧：明确动作对象是「图层树的图层/组」，不是画布上选中的要素
  //（要素删除在编辑工具条）。默认动作（右键菜单共用实例）同步改口。
  removeAction->setText(tr("删除所选图层/组"));
  removeAction->setToolTip(
      tr("删除图层树里选中的图层或组；画布上选中的要素用编辑工具条的「删除」"));

  // 工具条按钮走守卫包装：编辑会话中的图层先收尾（保存/放弃）再删——
  // 直接删会把未提交的编辑随图层析构静默丢弃。
  auto *guardedRemove = new QAction(tr("删除所选图层/组"), this);
  guardedRemove->setToolTip(removeAction->toolTip());
  connect(guardedRemove, &QAction::triggered, this, [this, removeAction] {
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

  auto mkButton = [bar](const QString &objectName, QAction *action) {
    auto *btn = new QToolButton(bar);
    btn->setObjectName(objectName);
    btn->setDefaultAction(action);
    btn->setToolTip(action->text());
    return btn;
  };
  lay->addWidget(mkButton(QStringLiteral("layerTreeAddGroupButton"), m_addGroupAction));
  lay->addWidget(mkButton(QStringLiteral("layerTreeRemoveSelectedButton"), guardedRemove));

  auto *expandAct = new QAction(tr("展开全部"), this);
  connect(expandAct, &QAction::triggered, m_view, &QgsLayerTreeView::expandAllNodes);
  lay->addWidget(mkButton(QStringLiteral("layerTreeExpandAllButton"), expandAct));
  auto *collapseAct = new QAction(tr("折叠全部"), this);
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
  m_menu->addAction(acts->actionRenameGroupOrLayer(m_menu));
  m_menu->addAction(acts->actionRemoveGroupOrLayer(m_menu));
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
}

void LayerTreePanel::updatePaleoActionStates()
{
  QgsMapLayer *layer = m_view->layerTreeModel() ? m_view->currentLayer() : nullptr;
  m_propertiesAction->setEnabled(layer != nullptr);
  m_duplicateAction->setEnabled(layer != nullptr);
  m_exportStyleAction->setEnabled(layer != nullptr);
  m_importStyleAction->setEnabled(layer != nullptr);

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
  if (QgsLayerTreeLayer *node = model->rootGroup()->findLayer(layer->id()))
  {
    QgsLayerTreeNode *parent = node->parent();
    if (parent && parent != model->rootGroup() && QgsLayerTree::isGroup(parent))
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
  QDomDocument doc;
  QString err;
  layer->exportNamedStyle(doc, err);
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return;
  f.write(doc.toByteArray(2));
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
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return;
  QDomDocument doc;
  if (!doc.setContent(&f))
    return; // 坏 .qml：静默放弃导入（对话框流程可后续接消息栏）
  QString err;
  layer->importNamedStyle(doc, err);
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

  for (QgsLayerTreeLayer *node : layerNodes)
  {
    QgsMapLayer *layer = node->layer();
    if (!layer)
      continue;
    if (!layer->isValid())
    {
      auto *ind = new QgsLayerTreeViewIndicator(m_view);
      ind->setIcon(style()->standardIcon(QStyle::SP_MessageBoxWarning));
      ind->setToolTip(tr("图层源不可用"));
      m_view->addIndicator(node, ind);
    }
    const QString paleoId =
        layer->customProperty(QStringLiteral("paleoLayerId")).toString();
    if (m_layerService && !paleoId.isEmpty())
    {
      for (const LayerDeclaration &d : m_layerService->declared())
      {
        if (d.layerId != paleoId)
          continue;
        if (!d.horizon.isEmpty() && d.horizon != activeHorizon)
        {
          auto *ind = new QgsLayerTreeViewIndicator(m_view);
          ind->setIcon(greyDotIcon());
          ind->setToolTip(tr("该图层属于层位 %1（未激活）").arg(d.horizon));
          m_view->addIndicator(node, ind);
        }
        break;
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

void LayerTreePanel::recenterEmptyState()
{
  if (!m_emptyState || !m_view)
    return;
  m_emptyState->adjustSize();
  m_emptyState->move((m_view->width() - m_emptyState->width()) / 2,
                     (m_view->height() - m_emptyState->height()) / 2);
}

bool LayerTreePanel::eventFilter(QObject *obj, QEvent *ev)
{
  if (obj == m_view && ev->type() == QEvent::Resize)
    recenterEmptyState(); // 空态卡片随宿主树 resize 保持居中
  return QWidget::eventFilter(obj, ev);
}
