// 层：视图
#include "datagraphpage.h"
#include "../../catalog/datacatalog.h"
#include "../../services/previewdoc.h"
#include "../../workflow/datagraphworkflow.h"
#include "../paleoicons.h"
#include "../paleotheme.h"
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsObject>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QStyleOptionGraphicsItem>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <cmath>

using namespace paleo::datagraph;
namespace {
class ElideLabel final : public QLabel {
public:
  using QLabel::QLabel;
  QSize minimumSizeHint() const override {
    return QSize(0, QLabel::minimumSizeHint().height());
  }
  QSize sizeHint() const override {
    return QSize(160, QLabel::sizeHint().height());
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setPen(palette().color(QPalette::WindowText));
    painter.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(font()).elidedText(text(), Qt::ElideRight,
                                                     width()));
  }
};
class NodeItem final : public QGraphicsObject {
public:
  Node node;
  std::function<void(const Node &, bool)> selected;
  std::function<void(const Node &)> activated;
  explicit NodeItem(const Node &n) : node(n) {
    setFlags(ItemIsSelectable | ItemIsFocusable);
    setCursor(Qt::PointingHandCursor);
    setZValue(1);
    updateNode(n);
  }
  QRectF boundingRect() const override { return QRectF(0, 0, 260, 88); }
  void updateNode(const Node &n) {
    node = n;
    setPos(n.position);
    setToolTip(
        (n.title + QLatin1Char('\n') + n.detail + QLatin1Char('\n') + n.path)
            .toHtmlEscaped());
    update();
  }
  void paint(QPainter *p, const QStyleOptionGraphicsItem *,
             QWidget *) override {
    const auto &t = PaleoTheme::tokens();
    p->setPen(QPen(isSelected() || hasFocus()                    ? t.focusRing
                   : node.stale || node.unresolved || node.cycle ? t.warning
                                                                 : t.border,
                   isSelected() || hasFocus() ? 2 : 1));
    p->setBrush(node.kind == Kind::Group ? t.surfaceAlt : t.surface);
    p->drawRoundedRect(boundingRect().adjusted(1, 1, -1, -1), t.radiusSm,
                       t.radiusSm);
    QFont font = p->font();
    font.setPointSize(t.bodyPt);
    font.setBold(node.kind == Kind::Group);
    p->setFont(font);
    p->setPen(t.text);
    const qreal lod = QStyleOptionGraphicsItem::levelOfDetailFromTransform(
        p->worldTransform());
    if (lod < .35)
      return;
    p->drawText(
        QRectF(t.spacingSm, t.spacingSm, 244, 26), Qt::AlignVCenter,
        QFontMetrics(font).elidedText(node.title, Qt::ElideMiddle, 244));
    if (lod < .65)
      return;
    font.setBold(false);
    font.setPointSize(t.labelPt);
    p->setFont(font);
    p->setPen(t.textMuted);
    QString kind =
        node.kind == Kind::Entity
            ? QCoreApplication::translate("DataGraphPage", "实体")
        : node.kind == Kind::Asset
            ? QCoreApplication::translate("DataGraphPage", "文件资料")
        : node.kind == Kind::Version
            ? QCoreApplication::translate("DataGraphPage", "资料版本")
        : node.kind == Kind::Activity
            ? QCoreApplication::translate("DataGraphPage", "生成记录")
        : node.kind == Kind::Missing
            ? QCoreApplication::translate("DataGraphPage", "待核实")
        : node.kind == Kind::Evidence ? QCoreApplication::translate("DataGraphPage", "地质证据")
        : node.kind == Kind::Assertion ? QCoreApplication::translate("DataGraphPage", "地质判断")
        : node.kind == Kind::Interpretation ? QCoreApplication::translate("DataGraphPage", "地质解释")
        : node.kind == Kind::Feature ? QCoreApplication::translate("DataGraphPage", "地图图斑")
        : node.kind == Kind::ReviewEvent ? QCoreApplication::translate("DataGraphPage", "审核事件")
            : QCoreApplication::translate("DataGraphPage", "分组");
    p->drawText(QRectF(t.spacingSm, 36, 244, 19),
                QFontMetrics(font).elidedText(kind + QStringLiteral(" · ") +
                                                  node.detail,
                                              Qt::ElideRight, 244));
    QString state =
        node.kind == Kind::Asset ? stageText(node.stage) : node.path;
    if (node.stale)
      state += QCoreApplication::translate("DataGraphPage", " · 已过期");
    if (node.unresolved)
      state += QCoreApplication::translate("DataGraphPage", " · 待关联");
    p->drawText(QRectF(t.spacingSm, 58, 244, 19),
                QFontMetrics(font).elidedText(state, Qt::ElideMiddle, 244));
  }
  void mousePressEvent(QGraphicsSceneMouseEvent *e) override {
    QGraphicsObject::mousePressEvent(e);
    dispatch(false);
  }
  void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *e) override {
    e->accept();
    dispatch(true);
  }
  void keyPressEvent(QKeyEvent *e) override {
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
      dispatch(true);
      e->accept();
    } else if (e->key() == Qt::Key_Space) {
      dispatch(false);
      e->accept();
    } else
      QGraphicsObject::keyPressEvent(e);
  }

private:
  void dispatch(bool open) {
    // Selection can refresh other panels and eventually rebuild the scene;
    // leave item event stack first.
    const Node value = node;
    const auto pick = selected;
    const auto activate = activated;
    QTimer::singleShot(0, this, [value, pick, activate, open] {
      if (open) {
        if (activate)
          activate(value);
      } else if (pick)
        pick(value, true);
    });
  }
};
} // namespace
DataGraphPage::DataGraphPage(QWidget *parent)
    : QWidget(parent), m_workflow(new DataGraphWorkflow(this)) {
  setObjectName(QStringLiteral("dataGraphPage"));
  setAccessibleName(tr("资料关联与成果血缘图谱"));
  const auto &t = PaleoTheme::tokens();
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(t.spacingSm, t.spacingSm, t.spacingSm,
                             t.spacingSm);
  auto *bar = new QWidget(this);
  bar->setObjectName(QStringLiteral("dataGraphToolBar"));
  PaleoTheme::applyThemedStyleSheet(bar, [] {
    const auto &theme = PaleoTheme::tokens();
    return QStringLiteral(
               "#dataGraphToolBar { background: %1; border-bottom: 1px solid %2; }")
               .arg(theme.surfaceAlt.name(), theme.border.name()) +
           PaleoTheme::toolButtonStyleSheet();
  });
  auto *barLayout = new QHBoxLayout(bar);
  barLayout->setContentsMargins(t.spacingXs, t.spacingXs, t.spacingXs, t.spacingXs);
  barLayout->setSpacing(t.spacingXs);
  const auto iconButton = [bar](const char *name, const char *icon,
                                const QString &tip) {
    auto *button = new QToolButton(bar);
    button->setObjectName(QLatin1String(name));
    button->setIcon(PaleoIcons::qgisTheme(QLatin1String(icon)));
    button->setIconSize(PaleoIcons::toolbarSize());
    button->setAutoRaise(true);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    return button;
  };
  m_mode = new QComboBox(bar);
  m_mode->setObjectName("dataGraphMode");
  m_mode->setToolTip(tr("图谱模式"));
  m_mode->addItem(tr("工区概览"), int(Mode::Overview));
  m_mode->addItem(tr("资料关联"), int(Mode::Relations));
  m_mode->addItem(tr("成果追溯"), int(Mode::Lineage));
  m_mode->addItem(tr("实体目录"), int(Mode::Entities));
  m_mode->addItem(tr("下游影响"), int(Mode::Impact));
  m_category = new QComboBox(bar);
  m_category->setObjectName("dataGraphCategory");
  m_category->setToolTip(tr("资料类别"));
  m_category->addItem(tr("全部类别"), QString());
  for (const QString &c : {QStringLiteral("well"), QStringLiteral("seismic"),
                           QStringLiteral("framework"),
                           QStringLiteral("product"), QStringLiteral("misc")})
    m_category->addItem(categoryText(c), c);
  m_stage = new QComboBox(bar);
  m_stage->setObjectName("dataGraphStage");
  m_stage->setToolTip(tr("资料阶段"));
  m_stage->addItem(tr("全部阶段"), QString());
  for (const QString &s :
       {QStringLiteral("RAW"), QStringLiteral("INTERMEDIATE"),
        QStringLiteral("DERIVED"), QStringLiteral("OUTPUT"),
        QStringLiteral("NONE")})
    m_stage->addItem(stageText(s), s);
  m_evidenceTypeFilter = new QComboBox(bar);
  m_evidenceTypeFilter->setObjectName("graphEvidenceType");
  m_evidenceTypeFilter->setToolTip(tr("证据类型"));
  m_evidenceTypeFilter->addItem(tr("全部证据类型"), QString());
  for (const auto &entry : QList<QPair<QString, QString>>{
           {tr("井资料"), "well"}, {tr("地震资料"), "seismic"},
           {tr("单因素"), "single_factor"}, {tr("约束"), "constraint"},
           {tr("规则"), "rule"}, {tr("人工"), "manual"},
           {tr("参考"), "reference"}, {tr("计算"), "computation"}})
    m_evidenceTypeFilter->addItem(entry.first, entry.second);
  m_reviewFilter = new QComboBox(bar);
  m_reviewFilter->setObjectName("graphEvidenceReview");
  m_reviewFilter->setToolTip(tr("解释状态"));
  m_reviewFilter->addItem(tr("全部解释状态"), QString());
  for (const auto &entry : QList<QPair<QString, QString>>{
           {tr("拟议"), "Proposed"}, {tr("待复核"), "NeedsReview"},
           {tr("已确认"), "Confirmed"}, {tr("已驳回"), "Rejected"},
           {tr("争议"), "Disputed"}, {tr("已替代"), "Superseded"}})
    m_reviewFilter->addItem(entry.first, entry.second);
  m_search = new QLineEdit(bar);
  m_search->setObjectName("dataGraphSearch");
  m_search->setPlaceholderText(tr("搜索"));
  m_search->setToolTip(tr("搜索文件、路径或资料名称"));
  m_search->setClearButtonEnabled(true);
  m_search->setMinimumWidth(96);
  m_search->setMaximumWidth(220);
  m_search->addAction(PaleoIcons::qgisTheme(QStringLiteral("mActionFilter2.svg")),
                      QLineEdit::LeadingPosition);
  m_back = iconButton("dataGraphBack", "mActionUndo.svg", tr("返回上一图谱"));
  m_back->setEnabled(false);
  m_depth = new QSpinBox(bar);
  m_depth->setObjectName(QStringLiteral("dataGraphDepth"));
  m_depth->setRange(1, 6);
  m_depth->setValue(2);
  m_depth->setToolTip(tr("邻域层数"));
  m_depth->setAccessibleName(tr("邻域层数"));
  m_history = iconButton("dataGraphHistory", "mActionHistory.svg",
                         tr("包含历史版本"));
  m_history->setCheckable(true);
  m_evidencePage = new QSpinBox(bar);
  m_evidencePage->setObjectName("graphEvidencePage");
  m_evidencePage->setRange(1, 200);
  m_evidencePage->setToolTip(tr("证据页，每页最多 50 条关联"));
  m_evidencePage->setAccessibleName(tr("证据页"));
  auto *refreshButton =
      iconButton("dataGraphRefresh", "mActionRefresh.svg", tr("刷新图谱"));
  auto *fit = iconButton("dataGraphFit", "mActionZoomFullExtent.svg",
                         tr("适配视图"));
  auto *zoomIn = iconButton("dataGraphZoomIn", "mActionZoomIn.svg", tr("放大"));
  auto *zoomOut =
      iconButton("dataGraphZoomOut", "mActionZoomOut.svg", tr("缩小"));
  m_cancel = iconButton("dataGraphCancel", "mActionCancelEdits.svg",
                        tr("取消计算"));
  m_cancel->setEnabled(false);
  auto *governance =
      iconButton("dataGraphManage", "mActionOptions.svg", tr("资料管理"));
  auto *governanceMenu = new QMenu(governance);
  for (const auto &[title, action] :
       {qMakePair(tr("资料体检"), QStringLiteral("health")),
        qMakePair(tr("回收清单"), QStringLiteral("recycle")),
        qMakePair(tr("导入台账"), QStringLiteral("imports")),
        qMakePair(tr("存储治理"), QStringLiteral("storage"))})
    connect(governanceMenu->addAction(title), &QAction::triggered, this,
            [this, action] { emit managementRequested(action, {}, {}); });
  governance->setMenu(governanceMenu);
  governance->setPopupMode(QToolButton::InstantPopup);
  m_export = iconButton("dataGraphExport", "mActionSaveMapAsImage.svg",
                        tr("导出当前图谱"));
  m_summary = new ElideLabel(bar);
  m_summary->setObjectName(QStringLiteral("dataGraphSummary"));
  for (QWidget *widget : std::initializer_list<QWidget *>{
           m_mode, m_category, m_stage, m_evidenceTypeFilter, m_reviewFilter,
           m_search, m_back, m_depth, m_history, m_evidencePage, refreshButton,
           fit, zoomIn, zoomOut, m_cancel, governance, m_export})
    barLayout->addWidget(widget);
  barLayout->addWidget(m_summary, 1);
  layout->addWidget(bar);
  connect(m_back, &QToolButton::clicked, this, [this] {
    if (m_backHistory.isEmpty())
      return;
    const auto previous = m_backHistory.takeLast();
    m_restoring = true;
    {
      const QSignalBlocker a(m_mode), b(m_category), c(m_stage), d(m_search),
          e(m_depth), f(m_history), g(m_relationFilter);
      m_focus = previous.focus;
      m_selected = {};
      m_mode->setCurrentIndex(m_mode->findData(int(previous.mode)));
      m_category->setCurrentIndex(m_category->findData(previous.category));
      m_stage->setCurrentIndex(m_stage->findData(previous.stage));
      m_search->setText(previous.search);
      m_depth->setValue(previous.depth);
      m_history->setChecked(previous.history);
      m_featureEvidence = previous.featureEvidence;
      m_evidenceTypeFilter->setCurrentIndex(qMax(0, m_evidenceTypeFilter->findData(previous.evidenceType)));
      m_reviewFilter->setCurrentIndex(qMax(0, m_reviewFilter->findData(previous.reviewState)));
      m_relationFilter->setCurrentIndex(qMax(0, m_relationFilter->findData(previous.relation)));
      m_evidencePage->setValue(previous.evidenceOffset / 50 + 1);
    }
    request();
    m_restoring = false;
    m_back->setEnabled(!m_backHistory.isEmpty());
  });
  connect(m_export, &QToolButton::clicked, this, [this] {
    QFileDialog dialog(this, tr("导出当前图谱与生成证据"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setNameFilter(tr("JSON (*.json)"));
    dialog.setDefaultSuffix(QStringLiteral("json"));
    dialog.selectFile(QStringLiteral("data-graph.json"));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
      return;
    m_workflow->exportGraph(m_graph, dialog.selectedFiles().first());
  });
  connect(m_evidenceTypeFilter, &QComboBox::currentIndexChanged, this, [this] { m_evidencePage->setValue(1); request(); });
  connect(m_reviewFilter, &QComboBox::currentIndexChanged, this, [this] { m_evidencePage->setValue(1); request(); });
  connect(m_relationFilter, &QComboBox::currentIndexChanged, this, [this] { m_evidencePage->setValue(1); request(); });
  connect(m_evidencePage, &QSpinBox::valueChanged, this, [this] { request(); });
  auto *splitter = new QSplitter(this);
  splitter->setChildrenCollapsible(false);
  m_scene = new QGraphicsScene(this);
  m_view = new QGraphicsView(m_scene, splitter);
  m_view->setObjectName("dataGraphView");
  m_view->setRenderHint(QPainter::Antialiasing);
  m_view->setDragMode(QGraphicsView::ScrollHandDrag);
  m_view->setAccessibleName(tr("资料图谱画布"));
  auto *details = new QWidget(splitter);
  auto *dl = new QVBoxLayout(details);
  dl->setContentsMargins(t.spacingSm, 0, 0, 0);
  auto *detailTabs = new QTabWidget(details);
  dl->addWidget(detailTabs, 1);
  m_relations = new QTreeWidget(detailTabs);
  m_relations->setObjectName("dataGraphRelations");
  m_relations->setHeaderLabels({tr("方向"), tr("关系"), tr("关联资料")});
  m_relations->setRootIsDecorated(false);
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
  detailTabs->addTab(m_details, tr("资料 / 生成证据"));
  detailTabs->addTab(m_relations, tr("关系明细"));
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
        qMakePair(tr("编辑关联角色"), QStringLiteral("role")),
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
  for (auto *button : {m_open, m_locate, m_folder, m_related, m_trace, m_impact, m_map,
                       m_reference, m_wells, m_manage}) {
    actionGrid->addWidget(button, row / 2, row % 2);
    button->setEnabled(false);
    ++row;
  }
  actionGrid->addWidget(tasks, row / 2, row % 2);
  dl->addLayout(actionGrid);
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
  splitter->addWidget(m_view);
  splitter->addWidget(details);
  splitter->setStretchFactor(0, 3);
  splitter->setStretchFactor(1, 1);
  layout->addWidget(splitter, 1);
  m_status = new QLabel(this);
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  connect(m_workflow, &DataGraphWorkflow::ready, this, &DataGraphPage::render);
  connect(m_workflow, &DataGraphWorkflow::exportFinished, this,
          [this](const QString &path, const QString &error) {
            m_status->setText(error.isEmpty() ? tr("图谱已导出：%1").arg(path)
                                              : tr("图谱导出：%1").arg(error));
          });
  connect(m_workflow, &DataGraphWorkflow::statusChanged, this,
          [this](const QString &s, bool busy) {
            m_status->setText(!busy && !m_graphMessage.isEmpty()
                                  ? s + QStringLiteral(" · ") + m_graphMessage
                                  : s);
            m_busy = busy;
            m_cancel->setEnabled(busy);
            m_view->setEnabled(!busy);
            if (busy)
              for (auto *b : {m_open, m_locate, m_folder, m_trace, m_impact, m_related,
                              m_map, m_reference, m_wells, m_manage})
                b->setEnabled(false);
            else
              selectNode(m_selected, false);
            m_export->setEnabled(!busy && !m_graph.nodes.isEmpty());
          });
  for (auto *combo : {m_mode, m_category, m_stage})
    connect(combo, &QComboBox::currentIndexChanged, this,
            [this] { request(); });
  connect(m_search, &QLineEdit::textChanged, this, [this] { request(); });
  connect(m_depth, &QSpinBox::valueChanged, this, [this] { request(); });
  connect(m_history, &QToolButton::toggled, this, [this] { request(); });
  connect(refreshButton, &QToolButton::clicked, this, &DataGraphPage::refresh);
  connect(fit, &QToolButton::clicked, this, [this] {
    m_view->fitInView(m_scene->itemsBoundingRect().adjusted(-16, -16, 16, 16),
                      Qt::KeepAspectRatio);
  });
  connect(zoomIn, &QToolButton::clicked, this, [this] {
    if (m_view->transform().m11() < 4)
      m_view->scale(1.2, 1.2);
  });
  connect(zoomOut, &QToolButton::clicked, this, [this] {
    if (m_view->transform().m11() > .15)
      m_view->scale(1 / 1.2, 1 / 1.2);
  });
  connect(m_cancel, &QToolButton::clicked, m_workflow,
          &DataGraphWorkflow::cancel);
  connect(m_open, &QPushButton::clicked, this,
          [this] { activateNode(m_selected); });
  connect(m_locate, &QPushButton::clicked, this,
          [this] { selectNode(m_selected, true); });
  connect(m_trace, &QPushButton::clicked, this, [this] {
    if (m_selected.versionId.isEmpty() && m_selected.assetId.isEmpty())
      return;
    m_focus = m_selected.versionId.isEmpty() ? assetKey(m_selected.assetId)
                                             : versionKey(m_selected.versionId);
    {
      const QSignalBlocker block(m_mode);
      m_mode->setCurrentIndex(m_mode->findData(int(Mode::Lineage)));
    }
    request();
  });
  connect(m_folder, &QPushButton::clicked, this, [this] {
    const QString path = selectedPath();
    if (!path.isEmpty())
      QDesktopServices::openUrl(
          QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
  });
  PaleoTheme::applyThemedStyleSheet(this, [this] {
    const auto &tokens = PaleoTheme::tokens();
    m_scene->setBackgroundBrush(tokens.surface);
    m_view->viewport()->update();
    for (auto *edge : m_edges) {
      auto *path = static_cast<QGraphicsPathItem *>(edge);
      auto pen = path->pen();
      pen.setColor(path->data(3).toBool()   ? tokens.focusRing
                   : path->data(0).toBool() ? tokens.warning
                                            : tokens.textMuted);
      path->setPen(pen);
    }
    return QString();
  });
}
void DataGraphPage::setDocService(PreviewDocService *doc) {
  m_doc = doc;
  m_workflow->setContext(doc ? doc->catalog() : nullptr,
                         doc ? doc->taskService() : nullptr);
  refresh();
}
void DataGraphPage::setPresentation(const Presentation &presentation) {
  m_presentation = presentation;
  m_workflow->setPresentation(presentation);
  refresh();
}
void DataGraphPage::setSelection(const QString &entity, const QString &asset,
                                 const QString &version) {
  if (m_navigating)
    return;
  const QString key = !version.isEmpty()  ? versionKey(version)
                      : !asset.isEmpty()  ? assetKey(asset)
                      : !entity.isEmpty() ? entityKey(entity)
                                          : QString();
  if (m_focus == key)
    return;
  m_focus = key;
  if (m_mode->currentIndex() != 0)
    request();
}
void DataGraphPage::openRelations(const QString &asset, const QString &version,
                                  const QString &entity) {
  openFocused(asset, version, entity, Mode::Relations);
}
void DataGraphPage::openFocused(const QString &asset, const QString &version,
                                const QString &entity, Mode mode) {
  m_selected = {};
  m_focus = !version.isEmpty()  ? versionKey(version)
            : !asset.isEmpty()  ? assetKey(asset)
            : !entity.isEmpty() ? entityKey(entity)
                                : m_focus;
  QSignalBlocker a(m_category), b(m_stage), c(m_search), d(m_mode);
  m_category->setCurrentIndex(0);
  m_stage->setCurrentIndex(0);
  m_search->clear();
  m_mode->setCurrentIndex(
      m_focus.isEmpty() ? 0 : qMax(0, m_mode->findData(int(mode))));
  // 一次入口只发一次查询：返回栈只记录用户真正到过的图谱状态。
  request();
}
void DataGraphPage::refresh() {
  const QString path =
      m_doc && m_doc->catalog() ? m_doc->catalog()->catalogPath() : QString();
  if (m_projectPath != path) {
    m_projectPath = path;
    m_focus.clear();
    m_backHistory.clear();
    m_hasQuery = false;
    m_featureEvidence.clear();
    m_back->setEnabled(false);
    render({});
  }
  m_workflow->setContext(m_doc ? m_doc->catalog() : nullptr,
                         m_doc ? m_doc->taskService() : nullptr);
  request();
}
void DataGraphPage::request() {
  m_history->setEnabled(m_mode->currentIndex() == 1);
  m_stage->setEnabled(m_mode->currentData().toInt() != int(Mode::Entities));
  m_history->setToolTip(tr("资料关联视图可展开历史版本；成果追溯按实际版本血缘"
                           "显示，工区概览按当前版本统计。"));
  if (!isVisible())
    return;
  Query q;
  q.mode = Mode(m_mode->currentData().toInt());
  q.focus = m_focus;
  q.category = m_category->currentData().toString();
  q.stage =
      q.mode == Mode::Entities ? QString() : m_stage->currentData().toString();
  q.search = m_search->text().trimmed();
  q.depth = m_depth->value();
  q.history = m_history->isChecked();
  q.featureEvidence = m_featureEvidence;
  q.horizon = m_featureEvidence.value("horizon").toString();
  q.evidenceType = m_evidenceTypeFilter->currentData().toString();
  q.reviewState = m_reviewFilter->currentData().toString();
  q.relation = m_relationFilter->currentData().toString();
  q.evidenceOffset = (m_evidencePage->value() - 1) * 50;
  if (m_hasQuery && !m_restoring &&
      (q.focus != m_queryState.focus || q.mode != m_queryState.mode ||
       q.category != m_queryState.category || q.stage != m_queryState.stage)) {
    m_backHistory.append(m_queryState);
    if (m_backHistory.size() > 30)
      m_backHistory.removeFirst();
  }
  m_queryState = q;
  m_hasQuery = true;
  m_back->setEnabled(!m_backHistory.isEmpty());
  m_workflow->request(q);
}
void DataGraphPage::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  refresh();
}
void DataGraphPage::hideEvent(QHideEvent *event) {
  m_workflow->cancel();
  QWidget::hideEvent(event);
}
void DataGraphPage::render(const Graph &graph) {
  m_graph = graph;
  m_graphMessage = graph.message;
  QSet<QString> keep;
  for (const Node &n : graph.nodes)
    keep.insert(n.id);
  for (auto it = m_items.begin(); it != m_items.end();) {
    if (!keep.contains(it.key())) {
      delete it.value();
      it = m_items.erase(it);
    } else
      ++it;
  }
  for (const Node &n : graph.nodes) {
    auto *item = static_cast<NodeItem *>(m_items.value(n.id));
    if (!item) {
      item = new NodeItem(n);
      item->selected = [this](const Node &node, bool navigate) {
        selectNode(node, navigate);
      };
      item->activated = [this](const Node &node) { activateNode(node); };
      m_scene->addItem(item);
      m_items.insert(n.id, item);
    } else
      item->updateNode(n);
  }
  for (auto *e : m_edges)
    delete e;
  m_edges.clear();
  for (const Edge &e : graph.edges) {
    if (!m_items.contains(e.source) || !m_items.contains(e.target))
      continue;
    const QPointF start = m_items[e.source]->pos() + QPointF(260, 44),
                  end = m_items[e.target]->pos() + QPointF(0, 44);
    QPainterPath path;
    path.moveTo(start);
    if (end.x() <= start.x())
      path.cubicTo(start + QPointF(30, -70), end + QPointF(-30, -70), end);
    else
      path.cubicTo(start + QPointF(30, 0), end - QPointF(30, 0), end);
    const double angle = end.x() > start.x() ? 0 : std::atan2(70.0, 30.0);
    path.moveTo(end - QPointF(std::cos(angle - .5), std::sin(angle - .5)) * 8);
    path.lineTo(end);
    path.lineTo(end - QPointF(std::cos(angle + .5), std::sin(angle + .5)) * 8);
    QPen pen(e.unresolved ? PaleoTheme::tokens().warning
                          : PaleoTheme::tokens().textMuted,
             1);
    if (e.relation == Relation::EditBaseline ||
        e.relation == Relation::Rollback || e.unresolved)
      pen.setStyle(Qt::DashLine);
    auto *item = m_scene->addPath(path, pen);
    item->setToolTip(
        (e.label + QStringLiteral("\n依据：") + e.provenance).toHtmlEscaped());
    item->setData(0, e.unresolved);
    item->setData(1, e.source);
    item->setData(2, e.target);
    item->setZValue(0);
    m_edges.append(item);
  }
  m_scene->setSceneRect(
      m_scene->itemsBoundingRect().adjusted(-16, -16, 16, 16));
  bool found = false;
  for (const Node &n : graph.nodes)
    if (n.id == m_selected.id) {
      m_selected = n;
      found = true;
      break;
    }
  if (!found) {
    m_selected = {};
    for (const auto &n : graph.nodes)
      if (n.id == m_focus) {
        m_selected = n;
        break;
      }
  }
  selectNode(m_selected, false);
  m_summary->setText(
      tr("%1 实体 · %2 资料 · %3 版本 · %4 节点 / %5 关系 · 待关联 %6 · "
         "缺失 %7%8%9%10")
          .arg(graph.totalEntities)
          .arg(graph.totalAssets)
          .arg(graph.totalVersions)
          .arg(graph.nodes.size())
          .arg(graph.edges.size())
          .arg(graph.unresolved)
          .arg(graph.missingParents)
          .arg(graph.hidden
                   ? tr(" · 还有 %1 项未展开，请缩小筛选范围").arg(graph.hidden)
                   : QString())
          .arg(graph.hiddenEdges
                   ? tr(" · 还有 %1 条关系未绘制").arg(graph.hiddenEdges)
                   : QString())
          .arg(graph.cycles ? tr(" · %1 组血缘环需核查").arg(graph.cycles)
                            : QString()));
  m_summary->setToolTip(m_summary->text());
  if (!graph.message.isEmpty())
    m_status->setText(graph.message);
}
QString DataGraphPage::selectedPath() const {
  if (!m_doc || !m_doc->catalog() || m_selected.assetId.isEmpty() ||
      m_presentation.removedIds.contains(m_selected.assetId))
    return {};
  const auto v = m_selected.versionId.isEmpty()
                     ? m_doc->catalog()->currentVersion(m_selected.assetId)
                     : m_doc->catalog()->versionById(m_selected.versionId);
  return v.id.isEmpty() ? QString() : m_doc->absolutePathForVersion(v);
}
void DataGraphPage::selectNode(const Node &node, bool navigate) {
  m_selected = node;
  m_relations->clear();
  QHash<QString, QString> titles;
  for (const auto &n : m_graph.nodes)
    titles.insert(n.id, n.title);
  for (const auto &e : m_graph.edges)
    if (e.source == node.id || e.target == node.id) {
      const auto other = e.source == node.id ? e.target : e.source;
      auto *row = new QTreeWidgetItem(
          m_relations, {e.source == node.id ? tr("流出 →") : tr("流入 ←"),
                        e.label.isEmpty() ? tr("分组成员") : e.label,
                        titles.value(other, other)});
      row->setData(0, Qt::UserRole, other);
      row->setToolTip(1, e.provenance);
      row->setToolTip(2, other);
    }
  for (auto it = m_items.cbegin(); it != m_items.cend(); ++it)
    it.value()->setSelected(it.key() == node.id);
  for (auto *edge : m_edges) {
    auto *path = static_cast<QGraphicsPathItem *>(edge);
    const bool related =
        !node.id.isEmpty() && (path->data(1).toString() == node.id ||
                               path->data(2).toString() == node.id);
    auto pen = path->pen();
    pen.setWidth(related ? 2 : 1);
    pen.setColor(related                  ? PaleoTheme::tokens().focusRing
                 : path->data(0).toBool() ? PaleoTheme::tokens().warning
                                          : PaleoTheme::tokens().textMuted);
    path->setData(3, related);
    path->setPen(pen);
  }
  m_details->setPlainText(
      node.id.isEmpty()
          ? QString()
          : node.title + QStringLiteral("\n\n") + node.detail +
                QStringLiteral("\n\n") +
                tr("类别：%1\n阶段：%2\n文件：%3\n资产 ID：%4\n版本 ID：%5")
                    .arg(
                        [&] {
                          QStringList names;
                          for (const auto &c : node.categories)
                            names << categoryText(c);
                          return names.join(QStringLiteral(" / "));
                        }(),
                        node.kind == Kind::Entity ? tr("不适用")
                        : node.kind == Kind::Group && node.stage.isEmpty()
                            ? tr("全部阶段")
                            : stageText(node.stage),
                        selectedPath(), node.assetId, node.versionId) +
                tr("\n导入来源：%1\nSHA-256：%2")
                    .arg(node.sourceUri, node.sha256) +
                (node.evidenceText.isEmpty()
                     ? QString()
                     : tr("\n\n生成证据（输入、模型、参数、支撑与软件）：\n") +
                           node.evidenceText));
  const bool real = !m_busy && node.kind != Kind::Missing &&
                    node.kind != Kind::Group &&
                    !m_presentation.removedIds.contains(node.assetId) &&
                    !m_presentation.removedIds.contains(node.entityId);
  m_open->setEnabled(real && !node.assetId.isEmpty());
  m_locate->setEnabled(real &&
                       (!node.assetId.isEmpty() || !node.entityId.isEmpty()));
  m_folder->setEnabled(real && !selectedPath().isEmpty());
  m_trace->setEnabled(real && !node.assetId.isEmpty());
  m_impact->setEnabled(real && (!node.versionId.isEmpty() || !node.evidence.value("source_version").toString().isEmpty()));
  m_impact->setToolTip(m_impact->isEnabled() ? tr("查询已登记的直接依赖和派生下游；空间影响未确定的成果显示为待复核风险") : tr("请选择具有已保存来源版本的节点"));
  m_related->setEnabled(real && !node.id.isEmpty());
  const auto version =
      m_doc && m_doc->catalog() && real
          ? (node.versionId.isEmpty()
                 ? m_doc->catalog()->currentVersion(node.assetId)
                 : m_doc->catalog()->versionById(node.versionId))
          : CatalogVersion();
  const bool product = version.extra.value("mapping_product").toBool() &&
                       (version.extra.value("layer_type") == "vector" ||
                        version.extra.value("layer_type") == "raster");
  m_map->setEnabled(product || (real && !node.featureUid.isEmpty()));
  m_reference->setEnabled(product);
  m_wells->setEnabled(
      product &&
      (version.extra.value("kind") == "wells_prediction" ||
       version.extra.value("result_kind") == "formation_prediction"));
  m_manage->setEnabled(real && !node.assetId.isEmpty());
  for (auto *b : {m_map, m_reference, m_wells})
    b->setToolTip(b->isEnabled() ? tr("打开当前选择的真实保存版本")
                                 : tr("请选择已登记的相应图件版本"));
  if (!navigate || !real)
    return;
  if (!node.featureUid.isEmpty()) {
    emit evidenceTargetRequested(nodeTarget(node));
    return;
  }
  if (node.kind == Kind::Version && !node.versionId.isEmpty())
    emit evidenceTargetRequested(nodeTarget(node));
  QScopedValueRollback<bool> guard(m_navigating, true);
  if (!node.versionId.isEmpty())
    emit versionFocusRequested(node.assetId, node.versionId);
  else if (!node.assetId.isEmpty())
    emit assetFocusRequested(node.assetId);
  else if (!node.entityId.isEmpty()) {
    emit entityFocusRequested(node.entityId);
    if (node.detail == QLatin1String("well"))
      emit wellSelected(node.entityId);
  }
}
void DataGraphPage::activateNode(const Node &node) {
  if (m_busy || node.id.isEmpty() || node.kind == Kind::Missing ||
      m_presentation.removedIds.contains(node.assetId) ||
      m_presentation.removedIds.contains(node.entityId))
    return;
  if (node.id == QLatin1String("group:entities")) {
    {
      const QSignalBlocker block(m_mode);
      m_mode->setCurrentIndex(m_mode->findData(int(Mode::Entities)));
    }
    request();
    return;
  }
  if (node.kind == Kind::Entity) {
    selectNode(node, true);
    m_focus = node.id;
    {
      const QSignalBlocker block(m_mode);
      m_mode->setCurrentIndex(m_mode->findData(int(Mode::Relations)));
    }
    request();
    return;
  }
  if (node.kind == Kind::Group) {
    QSignalBlocker a(m_category), b(m_stage), c(m_mode);
    m_mode->setCurrentIndex(0);
    m_category->setCurrentIndex(m_category->findData(node.categories.value(0)));
    m_stage->setCurrentIndex(
        node.stage.isEmpty() ? 0 : m_stage->findData(node.stage));
    request();
    return;
  }
  selectNode(node, true);
  if (node.kind == Kind::Evidence) {
    const auto source = node.evidence.value("source_version", node.evidence.value("version_id")).toString();
    if (!source.isEmpty()) emit versionActivated(source);
    return;
  }
  if (!node.versionId.isEmpty())
    emit versionActivated(node.versionId);
  else if (!node.assetId.isEmpty())
    emit assetActivated(node.assetId);
}

void DataGraphPage::openLineage(const QString &assetId,
                                const QString &versionId) {
  // 旧实现先按「资料关联」发一次查询再切「成果追溯」：中间态被压进返回栈，
  // 「返回上一图谱」落到一个用户从未请求过的关联视图。改为一次直达。
  openFocused(assetId, versionId, QString(), Mode::Lineage);
}
QVariantMap DataGraphPage::nodeTarget(const Node &node) const {
  auto out = node.evidence;
  out.insert("version_id", node.versionId); out.insert("asset_id", node.assetId);
  out.insert("layer_id", node.layerId); out.insert("feature_uid", node.featureUid);
  out.insert("evidence_id", node.evidenceId); out.insert("assertion_id", node.assertionId);
  out.insert("interpretation_id", node.interpretationId); out.insert("horizon", node.horizon);
  if (m_featureEvidence.contains("project_id")) out.insert("project_id", m_featureEvidence.value("project_id"));
  if (m_featureEvidence.contains("project_session")) out.insert("project_session", m_featureEvidence.value("project_session"));
  return out;
}
void DataGraphPage::setFeatureEvidence(const QVariantMap &snapshot, bool open) {
  if (m_featureEvidence == snapshot && !open) return;
  m_featureEvidence = snapshot;
  if (open) {
    const QSignalBlocker mode(m_mode), depth(m_depth), page(m_evidencePage);
    m_focus = versionKey(snapshot.value("version_id").toString());
    m_mode->setCurrentIndex(int(Mode::Relations)); m_depth->setValue(4); m_evidencePage->setValue(1);
  }
  request();
}
