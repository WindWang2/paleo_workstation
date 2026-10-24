// 层：视图
#include "datagraphpage.h"
#include "datagraphcanvas.h"
#include "../paleotheme.h"
#include "../../services/previewdoc.h"
#include "../../catalog/datacatalog.h"
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHeaderView>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QComboBox>
#include <QSignalBlocker>
#include <QMenu>
#include <QLabel>
#include <QSplitter>
#include <QTimer>
using namespace paleo::datagraph;
QWidget *DataGraphPage::createGraphPanels() {
  const auto &t = PaleoTheme::tokens();
  auto *splitter = new QSplitter(this);
  splitter->setObjectName(QStringLiteral("dataGraphSplitter"));
  m_scene = new QGraphicsScene(this);
  auto *canvas = new DataGraphCanvas(m_scene, splitter);
  canvas->clearFocusRequested = [this] { selectNode({}, false); };
  m_view = canvas;
  m_view->setObjectName("dataGraphView");
  m_view->setRenderHint(QPainter::Antialiasing);
  m_view->setDragMode(QGraphicsView::ScrollHandDrag);
  m_view->setAccessibleName(tr("资料图谱画布"));
  // 空场景下 QGraphicsView 的 sizeHint 极小，分栏的拉伸权重 = 初始尺寸 ×
  // stretch——先给画布一个初始尺寸意见，图谱才能拿到主区；宽度之后由用户拖动决定。
  m_view->resize(760, 600);
  auto *details = new QWidget(splitter); details->setMinimumWidth(200);
  details->setObjectName(QStringLiteral("dataGraphDetailsPanel"));
  auto *relations = new QWidget(splitter); relations->setMinimumWidth(200);
  relations->setObjectName(QStringLiteral("dataGraphRelationsPanel"));
  auto *rl = new QVBoxLayout(relations);
  rl->setContentsMargins(t.spacingSm, 0, 0, 0);
  auto *dl = new QVBoxLayout(details);
  dl->setContentsMargins(t.spacingSm, 0, 0, 0);
  dl->addWidget(new QLabel(tr("节点详情"), details));
  rl->addWidget(new QLabel(tr("关联关系"), relations));
  m_relations = new QTreeWidget(relations);
  rl->addWidget(m_relations, 1);
  m_relations->setObjectName("dataGraphRelations");
  m_relations->setHeaderLabels({tr("方向"), tr("关系"), tr("关联资料")});
  m_relations->setRootIsDecorated(false);
  m_relations->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_relations->header()->resizeSection(1, 88);
  m_relations->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  m_relations->setEditTriggers(QAbstractItemView::NoEditTriggers);
  connect(m_relations, &QTreeWidget::itemActivated, this,
          [this](QTreeWidgetItem *item) {
            const auto id = item->data(0, Qt::UserRole).toString();
            QTimer::singleShot(0, this, [this, id] {
              for (const auto &node : m_graph.nodes)
                if (node.id == id) {
                  selectNode(node, true);
                  if (auto *target = m_items.value(id))
                    m_view->centerOn(target);
                  break;
                }
            });
          });
  m_details = new QPlainTextEdit(details);
  m_details->setReadOnly(true);
  m_details->setPlaceholderText(tr(
      "选择节点查看资料、版本、文件路径与关联。双击分组展开，双击文件预览。"));
  m_details->setObjectName(QStringLiteral("dataGraphNodeDetails"));
  dl->addWidget(m_details, 1);
  m_open = new QPushButton(tr("预览文件 / 版本"), details);
  m_locate = new QPushButton(tr("在数据管理中定位"), details);
  m_folder = new QPushButton(tr("打开文件所在目录"), details);
  m_trace = new QPushButton(tr("追溯这个成果"), details);
  m_impact = new QPushButton(tr("查看来源的下游影响"), details);
  m_impact->setObjectName("dataGraphImpact");
  m_related = new QPushButton(tr("展开这个节点的关联"), details);
  m_map = new QPushButton(tr("在地图 / 图件工作台中显示"), details);
  m_reference = new QPushButton(tr("打开联动参考图"), details);
  m_wells = new QPushButton(tr("查看 / 修订测井预测相"), details);
  auto *tasks = new QPushButton(tr("打开任务中心"), details);
  m_manage = new QPushButton(tr("文件 / 版本操作"), details);
  auto *manageMenu = new QMenu(m_manage);
  for (const auto &[title, action] :
       {qMakePair(tr("版本管理 / 对比 / 回滚"), QStringLiteral("versions")),
        qMakePair(tr("调整资料归属与用途"), QStringLiteral("role")),
        qMakePair(tr("添加标签"), QStringLiteral("tag")),
        qMakePair(tr("移入回收清单"), QStringLiteral("remove"))})
    connect(manageMenu->addAction(title), &QAction::triggered, this,
            [this, action, manageMenu] {
              const auto asset = manageMenu->property("asset_id").toString();
              if (m_presentation.removedIds.contains(asset))
                return;
              emit managementRequested(
                  action, asset, manageMenu->property("version_id").toString());
            });
  connect(manageMenu, &QMenu::aboutToShow, this, [this, manageMenu] {
    manageMenu->setProperty("asset_id", m_selected.assetId);
    manageMenu->setProperty("version_id", m_selected.versionId);
  });
  m_manage->setMenu(manageMenu);
  auto *actionGrid = new QGridLayout;
  int row = 0;
  for (auto *button : {m_open, m_locate, m_folder}) { dl->addWidget(button); button->setEnabled(false); }
  for (auto *button : {m_related, m_trace, m_impact, m_map,
                       m_reference, m_wells, m_manage}) {
    actionGrid->addWidget(button, row, 0);
    button->setEnabled(false);
    ++row;
  }
  actionGrid->addWidget(tasks, row, 0);
  rl->addLayout(actionGrid);
  connect(tasks, &QPushButton::clicked, this,
          &DataGraphPage::taskCenterRequested);
  connect(m_impact, &QPushButton::clicked, this, [this] {
    const auto version = m_selected.evidence.value("source_version", m_selected.versionId).toString();
    if (version.isEmpty()) return;
    m_focus = versionKey(version);
    {
      // 模式切换由下方显式 request 统一发起：不屏蔽信号会同事件双发查询。
      const QSignalBlocker block(m_mode);
      m_mode->setCurrentIndex(m_mode->findData(int(Mode::Impact)));
    }
    request();
  });
  connect(m_related, &QPushButton::clicked, this, [this] {
    if (m_selected.id.isEmpty())
      return;
    m_focus = m_selected.id;
    {
      const QSignalBlocker block(m_mode);
      m_mode->setCurrentIndex(m_mode->findData(int(Mode::Relations)));
    }
    request();
  });
  for (const auto &[button, action] :
       {qMakePair(m_map, QStringLiteral("show")),
        qMakePair(m_reference, QStringLiteral("compare")),
        qMakePair(m_wells, QStringLiteral("welltracks"))})
    connect(button, &QPushButton::clicked, this, [this, action] {
      if (action == "show" && !m_selected.featureUid.isEmpty()) {
        emit evidenceTargetRequested(nodeTarget(m_selected)); return;
      }
      if (!m_doc || !m_doc->catalog())
        return;
      const auto version =
          m_selected.versionId.isEmpty()
              ? m_doc->catalog()->currentVersion(m_selected.assetId)
              : m_doc->catalog()->versionById(m_selected.versionId);
      if (!version.id.isEmpty())
        emit workspaceRequested(version.id, action);
    });
  splitter->addWidget(details);
  splitter->addWidget(m_view);
  splitter->addWidget(relations);
  splitter->setCollapsible(1, false);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setStretchFactor(2, 0);
  splitter->setSizes({240, 760, 240});
  return splitter;
}
