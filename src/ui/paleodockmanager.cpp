// 层：视图
#include "paleodockmanager.h"
#include "paleotheme.h"

#include <QApplication>
#include <QDockWidget>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QRubberBand>
#include <QScrollArea>
#include <QSettings>
#include <QTabWidget>
#include <QCursor>

namespace {
// Constant hints isolate dock geometry from changing tables, page stacks and
// asynchronously populated forms. The viewport, not the dock, absorbs overflow.
class DockScrollArea final : public QScrollArea
{
public:
  explicit DockScrollArea(QWidget *parent) : QScrollArea(parent)
  {
    setObjectName(QStringLiteral("dockScrollHost"));
    setProperty("paleo.scrollHost", true);
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  }
  QSize sizeHint() const override { return {320, 240}; }
  QSize minimumSizeHint() const override { return {120, 80}; }
};
constexpr Qt::DockWidgetArea areas[] = {Qt::LeftDockWidgetArea, Qt::RightDockWidgetArea,
                                       Qt::TopDockWidgetArea, Qt::BottomDockWidgetArea};
}

PaleoDockManager::PaleoDockManager(QMainWindow *window, const QString &settingsKey)
  : QObject(window), m_window(window), m_settingsKey(settingsKey),
    m_preview(new QRubberBand(QRubberBand::Rectangle, window))
{
  setObjectName(QStringLiteral("dockLayoutManager"));
  window->setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
  window->setAnimated(false); // Native drop preview must follow the pointer immediately.
  window->setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::South);
  m_preview->setAttribute(Qt::WA_TransparentForMouseEvents);
  const QStringList labels = {tr("← 左侧停靠"), tr("右侧停靠 →"),
                              tr("↑ 顶部停靠"), tr("↓ 底部停靠")};
  for (const QString &label : labels) {
    auto *guide = new QLabel(label, window);
    guide->setObjectName(QStringLiteral("dockDropGuide"));
    guide->setAlignment(Qt::AlignCenter);
    guide->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint |
                         Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
    guide->setAttribute(Qt::WA_ShowWithoutActivating);
    guide->setAttribute(Qt::WA_TransparentForMouseEvents);
    guide->hide();
    m_guides.append(guide);
  }
  m_dragTimer.setInterval(16);
  connect(&m_dragTimer, &QTimer::timeout, this, &PaleoDockManager::updateDrag);
  qApp->installEventFilter(this);
}

void PaleoDockManager::addDock(Qt::DockWidgetArea area, QDockWidget *dock)
{
  if (!dock)
    return;
  if (m_docks.contains(dock)) {
    m_window->addDockWidget(area, dock);
    return;
  }
  dock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
                    QDockWidget::DockWidgetFloatable);
  dock->setAllowedAreas(Qt::AllDockWidgetAreas);
  if (QWidget *content = dock->widget(); content && !content->property("paleo.scrollHost").toBool()) {
    auto *scroll = new DockScrollArea(dock);
    scroll->setWidget(content);
    dock->setWidget(scroll);
  }
  m_docks.append(dock);
  dock->setProperty("paleo.defaultDockArea", int(area));
  // Qt keeps placeholders for docks constructed after restoreState (e.g. wells).
  // Restore BEFORE adding: addDockWidget would consume the saved placeholder.
  dock->setParent(m_window);
  if (!m_window->restoreDockWidget(dock))
    m_window->addDockWidget(area, dock);
}

void PaleoDockManager::removeDock(QDockWidget *dock)
{
  if (m_dragDock == dock)
    endDrag(false);
  m_docks.removeAll(dock);
  m_window->removeDockWidget(dock);
}

void PaleoDockManager::captureDefaultLayout()
{
  m_defaultState = m_window->saveState();
}

void PaleoDockManager::restoreDefaultLayout()
{
  if (m_defaultState.isEmpty())
    return;
  // Late-created panels are not part of the initial snapshot.
  for (const auto &dock : m_docks)
    if (dock) {
      dock->setFloating(false);
      m_window->addDockWidget(Qt::DockWidgetArea(dock->property("paleo.defaultDockArea").toInt()), dock);
      dock->hide();
    }
  m_window->restoreState(m_defaultState);
}

void PaleoDockManager::fitLayout()
{
  // An explicit user command; never connected to content or page changes.
  for (Qt::DockWidgetArea area : areas) {
    QList<QDockWidget *> docks;
    QList<int> sizes;
    const bool side = area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea;
    const int available = side ? m_window->width() : m_window->height();
    for (const auto &dock : m_docks)
      if (dock && dock->isVisible() && !dock->isFloating() && m_window->dockWidgetArea(dock) == area) {
        docks.append(dock);
        sizes.append(qBound(120, available / 4, side ? 360 : 280));
      }
    if (!docks.isEmpty())
      m_window->resizeDocks(docks, sizes, side ? Qt::Horizontal : Qt::Vertical);
  }
}

QMenu *PaleoDockManager::createMenu(QWidget *parent)
{
  auto *menu = new QMenu(tr("布局与面板"), parent ? parent : m_window);
  menu->setObjectName(QStringLiteral("layoutManagementMenu"));
  menu->addAction(tr("自适应布局"), this, &PaleoDockManager::fitLayout);
  menu->addAction(tr("保存当前布局"), this, [this] {
    QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
    settings.setValue(m_settingsKey, m_window->saveState());
  });
  auto *restore = menu->addAction(tr("恢复保存的布局"), this, [this] {
    QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
    m_window->restoreState(settings.value(m_settingsKey).toByteArray());
  });
  QSettings settings(QStringLiteral("paleo"), QStringLiteral("paleo"));
  restore->setEnabled(settings.contains(m_settingsKey));
  restore->setToolTip(restore->isEnabled() ? tr("恢复上次手动保存的面板位置与尺寸")
                                         : tr("请先保存当前布局"));
  menu->addAction(tr("恢复默认布局"), this, &PaleoDockManager::restoreDefaultLayout);
  menu->addSeparator();
  for (const auto &dock : m_docks) {
    if (!dock)
      continue;
    menu->addAction(dock->toggleViewAction());
  }
  auto *positions = menu->addMenu(tr("移动面板"));
  const QStringList labels = {tr("停靠到左侧"), tr("停靠到右侧"), tr("停靠到顶部"), tr("停靠到底部")};
  for (const auto &dock : m_docks) {
    if (!dock)
      continue;
    auto *panel = positions->addMenu(dock->windowTitle());
    panel->addAction(tr("浮动窗口"), dock, [dock] {
      dock->setFloating(true);
      dock->show();
      dock->raise();
    });
    for (int i = 0; i < 4; ++i)
      panel->addAction(labels[i], dock, [this, dock, i] {
        dock->setFloating(false);
        m_window->addDockWidget(areas[i], dock);
        dock->show();
        dock->raise();
      });
  }
  // Visible nested preview workspaces use the same management entry point.
  for (auto *nested : m_window->findChildren<PaleoDockManager *>())
    if (nested != this && nested->m_window->isVisible()) {
      auto *sub = nested->createMenu(menu);
      sub->setTitle(nested->m_window->windowTitle());
      menu->addMenu(sub);
    }
  return menu;
}

bool PaleoDockManager::eventFilter(QObject *object, QEvent *event)
{
  // Floating panels belong to their preview tab, not to every preview at once.
  if (object == m_window && !m_window->isWindow()) {
    if (event->type() == QEvent::Hide)
      for (const auto &panel : m_docks)
        if (panel && panel->isFloating() && !panel->isHidden()) {
          panel->setProperty("paleo.resumeFloating", true);
          panel->hide();
        }
    if (event->type() == QEvent::Show)
      for (const auto &panel : m_docks)
        if (panel && panel->property("paleo.resumeFloating").toBool()) {
          panel->setProperty("paleo.resumeFloating", false);
          panel->show();
        }
  }
  auto *dock = qobject_cast<QDockWidget *>(object);
  if (dock && m_docks.contains(dock) &&
      (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::NonClientAreaMouseButtonPress)) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    // Only native title-bar drags, never scrolling or resizing the content.
    const QRect content = dock->widget() ? dock->widget()->geometry() : QRect();
    if (mouse->button() == Qt::LeftButton &&
        ((event->type() == QEvent::NonClientAreaMouseButtonPress &&
          mouse->position().y() < 0 && mouse->position().x() > 8 &&
          mouse->position().x() < dock->width() - 8) ||
         (event->type() == QEvent::MouseButtonPress &&
          mouse->position().y() < content.top() && !content.contains(mouse->position().toPoint())))) {
      m_dragDock = dock;
      m_pressPosition = mouse->globalPosition().toPoint();
      m_dragTimer.start();
    }
  }
  if (m_dragDock && event->type() == QEvent::KeyPress &&
      static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape)
    endDrag(false);
  if (m_dragDock && (event->type() == QEvent::MouseMove ||
                     (object == m_dragDock && event->type() == QEvent::Move)))
    updateDrag();
  if (m_dragDock && (event->type() == QEvent::MouseButtonRelease ||
                    event->type() == QEvent::NonClientAreaMouseButtonRelease))
    endDrag(true);
  return QObject::eventFilter(object, event);
}

void PaleoDockManager::updateDrag()
{
  if (!m_dragDock) {
    endDrag(false);
    return;
  }
  if (!(QApplication::mouseButtons() & Qt::LeftButton)) {
    endDrag(true);
    return;
  }
  const QPoint global = QCursor::pos();
  if (!m_dragging && (global - m_pressPosition).manhattanLength() < QApplication::startDragDistance())
    return;
  m_dragging = true;
  const QPoint pos = m_window->mapFromGlobal(global);
  // Exclude Ribbon/menu/status chrome. Guides use standard labels + rubber band;
  // splitting and tabbing elsewhere continue to use Qt's native drop preview.
  QRect bounds = m_window->rect();
  if (m_window->menuWidget())
    bounds.setTop(m_window->menuWidget()->geometry().bottom() + 1);
  bounds.adjust(8, 8, -8, -32);
  const QSize labelSize(qMax(112, m_guides.first()->sizeHint().width() + 16), 32);
  const QPoint c = bounds.center();
  const QRect targets[] = {
    QRect(QPoint(bounds.left(), c.y() - 16), labelSize),
    QRect(QPoint(bounds.right() - labelSize.width(), c.y() - 16), labelSize),
    QRect(QPoint(c.x() - labelSize.width() / 2, bounds.top()), labelSize),
    QRect(QPoint(c.x() - labelSize.width() / 2, bounds.bottom() - 32), labelSize)};
  m_target = Qt::NoDockWidgetArea;
  const bool allowed = m_window->isVisible() && bounds.contains(pos) &&
                       !(QApplication::keyboardModifiers() & Qt::ControlModifier);
  const auto &t = PaleoTheme::tokens(PaleoTheme::currentTheme());
  for (int i = 0; i < 4; ++i) {
    const bool active = allowed && targets[i].adjusted(-8, -8, 8, 8).contains(pos);
    auto *guide = m_guides[i];
    guide->setGeometry(QRect(m_window->mapToGlobal(targets[i].topLeft()), targets[i].size()));
    const QString style = QStringLiteral("QLabel { background: %1; color: %2; border: 1px solid %3; border-radius: 4px; }")
                          .arg((active ? t.primary : t.surface).name(),
                               (active ? t.onPrimary : t.text).name(), t.border.name());
    if (guide->styleSheet() != style)
      guide->setStyleSheet(style);
    guide->setVisible(allowed);
    guide->raise();
    if (active)
      m_target = areas[i];
  }
  QRect preview = bounds;
  if (m_target == Qt::LeftDockWidgetArea) preview.setWidth(bounds.width() / 4);
  if (m_target == Qt::RightDockWidgetArea) preview.setLeft(bounds.right() - bounds.width() / 4);
  if (m_target == Qt::TopDockWidgetArea) preview.setHeight(bounds.height() / 4);
  if (m_target == Qt::BottomDockWidgetArea) preview.setTop(bounds.bottom() - bounds.height() / 4);
  m_preview->setGeometry(preview);
  m_preview->setVisible(m_target != Qt::NoDockWidgetArea);
}

void PaleoDockManager::endDrag(bool commit)
{
  const auto dock = m_dragDock;
  const auto target = m_target;
  m_dragDock.clear();
  m_dragging = false;
  m_target = Qt::NoDockWidgetArea;
  m_dragTimer.stop();
  for (auto *guide : m_guides)
    guide->hide();
  m_preview->hide();
  // Finish after Qt's native release handler; a deleted/closed dock is harmless.
  if (commit && dock && target != Qt::NoDockWidgetArea)
    QTimer::singleShot(0, this, [this, dock, target] {
      if (!dock) return;
      dock->setFloating(false);
      m_window->addDockWidget(target, dock);
      dock->show();
      dock->raise();
    });
}
