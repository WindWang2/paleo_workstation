// 层：视图
#include "mappingreferencepanel.h"

#include "../../qgis/projectscope.h"
#include "../../qgis/referenceview.h"
#include "../decorations/paleodecorations.h"
#include "../paleotheme.h"

#include <QComboBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTimer>
#include <QToolButton>
#include <QHBoxLayout>
#include <QApplication>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QChildEvent>
#include <QLabel>
#include <QPushButton>
#include <QMouseEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include <functional>
#include <memory>
MappingReferencePanel::MappingReferencePanel(QgsMapCanvas *mainCanvas,
                                             QgsProject *project, QWidget *parent)
    : QWidget(parent)
{
  setObjectName(QStringLiteral("mappingReferencePanel"));
  auto *layout = new QVBoxLayout(this);
  const auto &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(tokens.spacingSm, tokens.spacingSm, tokens.spacingSm, tokens.spacingSm);
  layout->setSpacing(tokens.spacingXs);
  auto *header = new QHBoxLayout;
  header->setSpacing(tokens.spacingXs);
  m_sources = new QComboBox(this);
  m_sources->setObjectName(QStringLiteral("referenceSource"));
  m_sources->setAccessibleName(tr("参考证据"));
  m_sources->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_sources->setMinimumContentsLength(12);
  m_linked = new QToolButton(this);
  m_linked->setObjectName(QStringLiteral("referenceLinked"));
  m_linked->setText(tr("联动"));
  m_linked->setCheckable(true);
  m_linked->setChecked(true);
  m_linked->setAccessibleName(tr("与主画布联动"));
  m_linked->setToolTip(tr("打开后证据视图跟随主画布的比例尺和中心，不反向改主图；关闭后可独立浏览"));
  m_remove = new QToolButton(this);
  m_remove->setObjectName(QStringLiteral("referenceRemoveView"));
  m_remove->setText(QStringLiteral("−"));
  m_remove->setAccessibleName(tr("关闭此证据"));
  m_remove->setToolTip(tr("关闭此证据"));
  header->addWidget(m_sources, 1);
  header->addWidget(m_linked);
  header->addWidget(m_remove);
  layout->addLayout(header);
  m_empty = new QLabel(tr("从上方下拉框选择参考证据。\n"
                         "可选择地震相、测井相、单因素图或参考相图。"), this);
  m_empty->setObjectName(QStringLiteral("referenceEmptyState"));
  m_empty->setWordWrap(true);
  m_empty->setAlignment(Qt::AlignCenter);
  layout->addWidget(m_empty, 1);
  // #746：空视图不再各带一套 canvas/装饰器——容器先占位，首次选证据时
  // ensureView() 再创建（默认四个视图不再常驻四个 QgsMapCanvas）。
  m_mainCanvas = mainCanvas;
  m_project = project;
  m_viewHost = new QWidget(this);
  m_viewHost->setObjectName(QStringLiteral("referenceViewHost"));
  auto *hostLayout = new QVBoxLayout(m_viewHost);
  hostLayout->setContentsMargins(0, 0, 0, 0);
  hostLayout->setSpacing(0);
  layout->addWidget(m_viewHost, 1);
  m_viewHost->hide();
  connect(m_sources, &QComboBox::currentIndexChanged, this, [this] {
    clearReference();
    updateCaption();
    if (!sourceId().isEmpty()) emit referenceRequested(sourceId());
  });
  connect(m_linked, &QToolButton::toggled, this, &MappingReferencePanel::updateLink);
  connect(m_remove, &QToolButton::clicked, this, &MappingReferencePanel::removeViewRequested);
  setMinimumSize(260, 260);
  refreshSources();
}

void MappingReferencePanel::ensureView()
{
  if (m_view) return;
  m_view = createReferenceView(m_mainCanvas, m_project, m_viewHost);
  m_view->widget()->setMinimumSize(240, 160);
  m_viewHost->layout()->addWidget(m_view->widget());
  m_view->widget()->hide();
  m_decor = new PaleoDecorationManager(canvas());
  m_decor->setScaleBarEnabled(true);
  // 创建前已到达的显隐/联动状态补上，避免首个证据不跟随主图。
  m_view->setLinked(m_linked->isChecked());
  m_view->setViewActive(m_active);
  emit viewCreated(m_view->widget());
}

MappingReferencePanel::~MappingReferencePanel()
{
  if (!m_view) return;
  disconnect(m_view->widget(), nullptr, this, nullptr);
  m_view->clearReference();
}
QgsMapCanvas *MappingReferencePanel::canvas() const { return m_view ? m_view->canvas() : nullptr; }
QString MappingReferencePanel::sourceId() const { return m_sources->currentData().toString(); }
bool MappingReferencePanel::hasReference() const { return m_view && m_view->hasReference(); }

QString MappingReferencePanel::category(const QVariantMap &row)
{
  const auto kind = row.value("kind").toString();
  const auto id = row.value("id").toString();
  const auto metadata = row.value("parameters").toMap();
  if (kind.startsWith(QLatin1String("single_factor")) || kind == QLatin1String("contour_lines") ||
      !metadata.value("factor_id").toString().isEmpty() || id.startsWith(QLatin1String("wellfactor."))) return QStringLiteral("factor");
  if (kind.contains(QLatin1String("well")) || id.startsWith(QLatin1String("welllog.")) ||
      metadata.value("result_kind") == "formation_prediction")
    return QStringLiteral("well");
  if (kind.contains(QLatin1String("seismic"))) return QStringLiteral("seismic");
  return QStringLiteral("reference");
}

QVariantMap MappingReferencePanel::selectedSource() const
{
  for (const auto &row : m_rows)
    if (row.toMap().value("id").toString() == sourceId()) return row.toMap();
  return {};
}

void MappingReferencePanel::setSources(const QVariantList &sources, const QString &activeHorizon)
{
  m_rows = sources;
  m_activeHorizon = activeHorizon;
  refreshSources();
  updateCaption();
}

void MappingReferencePanel::refreshSources()
{
  const auto selected = sourceId();
  const QSignalBlocker blocker(m_sources);
  m_sources->clear();
  m_sources->addItem(tr("请选择参考证据"), QString());
  for (const auto &entry : m_rows) {
    const auto row = entry.toMap();
    if (row.value("type") != "vector" && row.value("type") != "raster") continue;
    const auto family = category(row);
    const auto familyName = family == QLatin1String("seismic") ? tr("地震相")
        : family == QLatin1String("well") ? tr("测井相")
        : family == QLatin1String("factor") ? tr("单因素图") : tr("参考图");
    const auto title = tr("%1 · %2").arg(familyName, row.value("name").toString());
    m_sources->addItem(title, row.value("id"));
    m_sources->setItemData(m_sources->count() - 1, row.value("path"), Qt::ToolTipRole);
  }
  m_sources->setCurrentIndex(qMax(0, m_sources->findData(selected)));
  m_sources->setEnabled(m_sources->count() > 1);
  m_sources->setToolTip(m_sources->isEnabled() ? tr("选择要对照的图件；同一面板可随时切换来源")
                                            : tr("暂无此类图件，请先导入参考相图或生成预测、单因素成果"));
  if (sourceId() != selected) clearReference();
  updateCaption();
}

void MappingReferencePanel::selectSource(const QString &id)
{
  const int index = m_sources->findData(id);
  if (index >= 0) {
    if (index == m_sources->currentIndex() && !hasReference() && !id.isEmpty())
      emit referenceRequested(id);
    else m_sources->setCurrentIndex(index);
  }
}

void MappingReferencePanel::setReference(QgsMapLayer *layer, const QVariantMap &metadata)
{
  clearReference();
  if (!layer) return;
  ensureView();
  m_metadata = metadata;
  m_empty->hide();
  m_viewHost->show();
  m_view->widget()->show();
  m_view->setReference(layer, metadata);
  updateCaption();
  setProperty("paleo.referenceVersion", selectedSource().value("version_id"));
  emit referenceChanged(selectedSource().value("name").toString(), selectedSource().value("version_id").toString());
}

void MappingReferencePanel::clearReference(const QString &message)
{
  if (m_view) {
    m_view->clearReference();
    m_view->widget()->hide();
  }
  if (m_viewHost) m_viewHost->hide();
  m_metadata.clear();
  m_empty->setText(message.isEmpty() ? tr("从上方下拉框选择参考证据。\n"
                                         "可选择地震相、测井相、单因素图或参考相图。") : message);
  m_empty->show();
  setProperty("paleo.referenceVersion", QString());
  emit referenceChanged(QString(), QString());
}

void MappingReferencePanel::updateCaption()
{
  // issue-743：组合框禁用态保留 refreshSources()(:138-139) 写的禁用原因文案
  // （「暂无此类图件…」）——否则 updateCaption 的来源备注会立即覆盖它，用户看不到
  // 为何禁用。
  if (!m_sources->isEnabled())
    return;
  const auto row = selectedSource();
  const auto horizon = row.value("horizon").toString();
  const QString note = row.isEmpty() ? tr("选择本视图的参考证据，与主图联动对照。")
      : !horizon.isEmpty() && !m_activeHorizon.isEmpty() && horizon != m_activeHorizon
          ? tr("参考层位：%1 · 当前绘制层位：%2（跨层位对照）").arg(horizon, m_activeHorizon)
          : tr("参考层位：%1 · 保留所选版本，与主图独立管理").arg(horizon.isEmpty() ? tr("共享资料") : horizon);
  m_sources->setToolTip(note);
}

void MappingReferencePanel::setViewActive(bool active)
{
  m_active = active;
  if (m_view) m_view->setViewActive(active);
}

void MappingReferencePanel::updateLink()
{
  if (m_view) m_view->setLinked(m_linked->isChecked());
}

// D-12（full-audit-20261020）：#547 裁决允许关闭最后一个视图——按钮恒可点，
// 仅当要关的是最后一个视图时换提示（原 setCanRemove 的 bool 是死参，恒
// setEnabled(true)，误导维护者以为存在「至少一视图」保护）。
void MappingReferencePanel::setRemoveHint(bool lastView)
{
  m_remove->setEnabled(true);
  m_remove->setToolTip(lastView ? tr("将关闭最后一个证据视图（可随时再添加）")
                                 : tr("关闭此证据"));
}

namespace {
// #745：宽度按工程作用域存——旧的全局键让 A 工程的布局影响 B 工程。
// 作用域键由 qgis 层给出（路径 SHA1），视图层不直连 qgsproject.h（qgs-audit）。
QString widthsKey(QgsProject *project)
{
  const QString scope = paleoProjectScopeKey(project);
  return QStringLiteral("mappingEvidence/viewWidths/")
      + (scope.isEmpty() ? QStringLiteral("unscoped") : scope);
}

// 证据视图右缘的拖动手柄：左右拖动即调整其左侧视图宽度。
class EvidenceResizeHandle : public QWidget
{
public:
  using Callback = std::function<void(int dx, bool finished)>;
  EvidenceResizeHandle(int width, Callback cb, QWidget *parent) : QWidget(parent), m_cb(std::move(cb))
  {
    setObjectName(QStringLiteral("referenceViewResizeHandle"));
    setFixedWidth(width);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setCursor(Qt::SplitHCursor);
    // issue-744：键盘与读屏可用——可获得焦点、方向键步进调宽、带无障碍名称。
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(QObject::tr("调整证据视图宽度"));
    setAccessibleDescription(QObject::tr("按左方向键收窄、右方向键加宽此证据视图"));
  }

protected:
  void mousePressEvent(QMouseEvent *e) override
  {
    if (e->button() != Qt::LeftButton) return QWidget::mousePressEvent(e);
    m_pressX = e->globalPosition().toPoint().x();
    m_dragging = true;
    m_cb(0, false);
    e->accept();
  }
  void mouseMoveEvent(QMouseEvent *e) override
  {
    if (!m_dragging) return QWidget::mouseMoveEvent(e);
    m_cb(e->globalPosition().toPoint().x() - m_pressX, false);
    e->accept();
  }
  void mouseReleaseEvent(QMouseEvent *e) override
  {
    if (!m_dragging) return QWidget::mouseReleaseEvent(e);
    m_dragging = false;
    m_cb(e->globalPosition().toPoint().x() - m_pressX, true);
    e->accept();
  }
  // issue-744：键盘步进调宽——先以当前宽度设起点（dx=0 回调记录 startWidth），
  // 再 ±kKeyStep（finished 落持久化），与拖动手感一致。
  void keyPressEvent(QKeyEvent *e) override
  {
    if (e->key() == Qt::Key_Left || e->key() == Qt::Key_Right)
    {
      m_cb(0, false);
      m_cb(e->key() == Qt::Key_Right ? kKeyStep : -kKeyStep, true);
      e->accept();
      return;
    }
    QWidget::keyPressEvent(e);
  }

private:
  static constexpr int kKeyStep = 20; // 每次方向键的宽度步进（像素）
  Callback m_cb;
  int m_pressX = 0;
  bool m_dragging = false;
};
} // namespace

MappingEvidencePanel::MappingEvidencePanel(QgsMapCanvas *mainCanvas, QgsProject *project,
                                           QWidget *parent)
    : QWidget(parent), m_mainCanvas(mainCanvas), m_project(project)
{
  setObjectName(QStringLiteral("mappingEvidencePanel"));
  setProperty("paleo.scrollHost", true); // 内部滚动仅作用于视图区，工具行保持可见。
  const auto &tokens = PaleoTheme::tokens();
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(tokens.spacingSm, tokens.spacingSm, tokens.spacingSm, tokens.spacingSm);
  layout->setSpacing(tokens.spacingXs);
  auto *tools = new QHBoxLayout;
  m_count = new QLabel(this);
  m_count->setObjectName(QStringLiteral("referenceViewCount"));
  auto *add = new QPushButton(tr("添加视图"), this);
  add->setObjectName(QStringLiteral("referenceAddView"));
  add->setToolTip(tr("在同一行添加参考视图，左右滚动查看更多证据"));
  tools->addWidget(m_count);
  tools->addStretch();
  tools->addWidget(add);
  layout->addLayout(tools);
  m_scroll = new QScrollArea(this);
  m_scroll->setObjectName(QStringLiteral("referenceViewsScroll"));
  m_scroll->setFrameShape(QFrame::NoFrame);
  m_scroll->setWidgetResizable(true);
  m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *body = new QWidget(m_scroll);
  body->setObjectName(QStringLiteral("referenceViewsRow"));
  body->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
  m_row = new QHBoxLayout(body);
  m_row->setContentsMargins(0, 0, 0, 0);
  m_row->setSpacing(0); // 视图间隙由拖动手柄承担
  m_row->setAlignment(Qt::AlignLeft);
  m_scroll->setWidget(body);
  layout->addWidget(m_scroll, 1);
  // D-12（full-audit-20261020）：空态引导——全部视图关闭后仅「0 个视图」
  // 计数与空白滚动区，新用户无从得知可恢复；§42.4 空态暖引导同仓既有口径。
  m_emptyHint = new QLabel(tr("无证据视图——点「添加视图」恢复对照"), this);
  m_emptyHint->setObjectName(QStringLiteral("referenceEmptyHint"));
  m_emptyHint->setWordWrap(true);
  m_emptyHint->setAlignment(Qt::AlignCenter);
  m_emptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);
  PaleoTheme::applyThemedStyleSheet(m_emptyHint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  m_emptyHint->hide();
  layout->addWidget(m_emptyHint);
  m_scroll->viewport()->installEventFilter(this);
  {
    const QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
    for (const auto &v : settings.value(widthsKey(m_project)).toList()) m_savedWidths << v.toInt();
  }
  connect(add, &QPushButton::clicked, this, [this] { addView(); });
  for (int i = 0; i < kDefaultViewCount; ++i) addView();
}

MappingReferencePanel *MappingEvidencePanel::addView()
{
  auto *view = new MappingReferencePanel(m_mainCanvas, m_project, m_scroll->widget());
  view->setSources(m_sources, m_horizon);
  view->setViewActive(m_active);
  const int index = int(m_views.size());
  m_views << view;
  if (index < m_savedWidths.size() && m_savedWidths[index] > 0)
    m_widths.insert(view, qBound(kMinViewWidth, m_savedWidths[index], kMaxViewWidth));
  auto startWidth = std::make_shared<int>(0);
  auto *handle = new EvidenceResizeHandle(qMax(6, PaleoTheme::tokens().spacingSm),
      [this, view, startWidth](int dx, bool finished) {
        if (dx == 0 && !finished) { *startWidth = view->width(); return; }
        setViewWidth(view, *startWidth + dx, finished);
      }, m_scroll->widget());
  handle->setToolTip(tr("左右拖动或聚焦后按左/右方向键调整此证据视图宽度"));
  handle->installEventFilter(this); // 手柄上的滚轮同样横向滚动
  m_handles << handle;
  view->installEventFilter(this);
  for (auto *child : view->findChildren<QWidget *>()) child->installEventFilter(this);
  connect(view, &MappingReferencePanel::referenceRequested, this, [this, view](const QString &id) {
    emit referenceRequested(view, id);
  });
  // #746：画布延迟创建，addView 时的子控件快照装不到它——创建后补装过滤器，
  // 否则滚轮落在 canvas viewport 上不会转成横向滚动。
  connect(view, &MappingReferencePanel::viewCreated, this, [this](QWidget *created) {
    created->installEventFilter(this);
    for (auto *child : created->findChildren<QWidget *>()) child->installEventFilter(this);
  });
  connect(view, &MappingReferencePanel::removeViewRequested, this, [this, view] { removeView(view); });
  layoutViews();
  QTimer::singleShot(0, view, [this, view] {
    if (m_views.contains(view)) m_scroll->ensureWidgetVisible(view);
  });
  return view;
}

void MappingEvidencePanel::removeView(MappingReferencePanel *view)
{
  const auto index = m_views.indexOf(view);
  if (index < 0) return;
  m_views.removeAt(index);
  m_widths.remove(view);
  // #745：宽度按序号存，删视图必须立刻落盘——否则后续视图的宽度依次左移套到
  // 别的视图上（直到用户下次拖动才纠正）。
  if (index < m_savedWidths.size()) m_savedWidths.removeAt(index);
  QWidget *handle = m_handles.takeAt(index);
  m_row->removeWidget(handle);
  handle->deleteLater();
  view->clearReference();
  m_row->removeWidget(view);
  view->hide();
  view->deleteLater();
  layoutViews();
  saveWidths(); // 用剩余视图重建序号表（已删项不留空洞）
}

void MappingEvidencePanel::layoutViews()
{
  while (QLayoutItem *item = m_row->takeAt(0))
    delete item;
  // All evidence views remain in one horizontal strip, including narrow docks.
  for (qsizetype i = 0; i < m_views.size(); ++i) {
    auto *view = m_views[i];
    view->setMinimumHeight(200);
    view->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    view->setRemoveHint(m_views.size() == 1);
    view->setAccessibleName(tr("证据视图 %1").arg(i + 1));
    m_row->addWidget(view);
    view->show();
    m_row->addWidget(m_handles[i]);
    m_handles[i]->show();
  }
  updateViewSizes();
  m_row->activate();
  m_count->setText(tr("%1 个视图").arg(m_views.size()));
  m_scroll->setVisible(!m_views.isEmpty()); // D-12：空态收起滚动区，展示引导
  m_emptyHint->setVisible(m_views.isEmpty());
}

int MappingEvidencePanel::defaultViewWidth() const
{
  // 未调整过的视图一屏约放四个；不足四个时均分，最窄 kMinViewWidth。
  const int per = qBound(1, int(m_views.size()), kDefaultViewCount);
  const int gap = m_handles.isEmpty() ? 0 : m_handles.first()->width();
  return qMax(kMinViewWidth, (m_scroll->viewport()->width() - per * gap) / per);
}

void MappingEvidencePanel::updateViewSizes()
{
  const int fallback = defaultViewWidth();
  int total = 0;
  for (qsizetype i = 0; i < m_views.size(); ++i) {
    const int width = m_widths.value(m_views[i], fallback);
    m_views[i]->setFixedWidth(width);
    total += width + m_handles[i]->width();
  }
  // 总宽超过视口时出现水平滚动条（ScrollBarAsNeeded）。
  m_scroll->widget()->setMinimumWidth(total);
  // issue-812：矮停靠栏视口高低于视图最小高(200，见 layoutViews)时，行高按 200
  // 兜底——否则视图被压到最小高以下、canvas 内容被裁。视口更矮时底部超出视口
  // （竖滚恒关，测试口径如此），至少保证视图按最小高完整渲染而非压扁。
  m_scroll->widget()->setFixedHeight(qMax(m_scroll->viewport()->height(), 200));
}

void MappingEvidencePanel::setViewWidth(MappingReferencePanel *view, int width, bool persist)
{
  if (!m_views.contains(view)) return;
  if (width <= 0) m_widths.remove(view);
  else m_widths.insert(view, qBound(kMinViewWidth, width, kMaxViewWidth));
  updateViewSizes();
  m_row->activate();
  if (persist) saveWidths();
}

int MappingEvidencePanel::viewWidth(MappingReferencePanel *view) const
{
  return m_views.contains(view) ? m_widths.value(view, defaultViewWidth()) : 0;
}

int MappingEvidencePanel::visibleViewCount() const
{
  const QRect port(m_scroll->widget()->mapFrom(m_scroll->viewport(), QPoint()), m_scroll->viewport()->size());
  int n = 0;
  for (auto *view : m_views)
    if (view->isVisibleTo(m_scroll->widget()) && port.intersects(view->geometry())) ++n;
  return n;
}

void MappingEvidencePanel::saveWidths()
{
  QVariantList widths;
  m_savedWidths.clear();
  for (auto *view : m_views) {
    m_savedWidths << m_widths.value(view, 0);
    widths << m_savedWidths.last();
  }
  QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
  settings.setValue(widthsKey(m_project), widths);
}

bool MappingEvidencePanel::eventFilter(QObject *watched, QEvent *event)
{
  // issue-747：视图内后续新增的子控件（如运行时弹出的控件）也要装滚轮过滤器
  // ——addView 时只对当时已有的子控件快照式安装，动态新增的会漏掉横向滚轮。
  if (event->type() == QEvent::ChildAdded)
  {
    if (auto *child = static_cast<QChildEvent *>(event)->child())
      if (auto *w = qobject_cast<QWidget *>(child))
        w->installEventFilter(this);
  }
  if (event->type() == QEvent::Resize && watched == m_scroll->viewport()) updateViewSizes();
  if (event->type() == QEvent::Wheel) {
    auto *wheel = static_cast<QWheelEvent *>(event);
    const bool horizontal = qAbs(wheel->pixelDelta().x()) > qAbs(wheel->pixelDelta().y()) ||
        qAbs(wheel->angleDelta().x()) > qAbs(wheel->angleDelta().y());
    if (horizontal || watched == m_scroll->viewport()) {
      auto *bar = m_scroll->horizontalScrollBar();
      const int pixels = horizontal ? wheel->pixelDelta().x() : wheel->pixelDelta().y();
      const int angle = horizontal ? wheel->angleDelta().x() : wheel->angleDelta().y();
      const int distance = pixels ? pixels : qRound(angle / 120.0 * QApplication::wheelScrollLines() * bar->singleStep());
      bar->setValue(bar->value() - distance);
      wheel->accept();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

void MappingEvidencePanel::setSources(const QVariantList &sources, const QString &horizon)
{
  m_sources = sources;
  m_horizon = horizon;
  for (auto *view : m_views) view->setSources(sources, horizon);
}

void MappingEvidencePanel::clearReferences(const QString &message)
{
  for (auto *view : m_views) view->clearReference(message);
}

void MappingEvidencePanel::setViewActive(bool active)
{
  m_active = active;
  for (auto *view : m_views) view->setViewActive(active);
}

MappingReferencePanel *MappingEvidencePanel::showSource(const QString &id)
{
  for (auto *view : m_views)
    if (view->sourceId() == id) {
      view->selectSource(id);
      m_scroll->ensureWidgetVisible(view);
      return view;
    }
  auto *view = [&]() -> MappingReferencePanel * {
    for (auto *candidate : m_views) if (candidate->sourceId().isEmpty()) return candidate;
    return addView();
  }();
  view->selectSource(id);
  m_scroll->ensureWidgetVisible(view);
  return view;
}
