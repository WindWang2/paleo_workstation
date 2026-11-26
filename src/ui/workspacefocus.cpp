// 层：视图
#include "workspacefocus.h"

#include "paleomainwindow.h"
#include "paleoicons.h"
#include "paleotheme.h"
#include "shortcuts/shortcutcatalog.h"

#include <QApplication>
#include <QBoxLayout>
#include <QKeyEvent>
#include <QShortcut>
#include <QTabBar>
#include <QToolButton>
#include <QTimer>

namespace {
QToolButton *button(QWidget *parent, const char *name, const QString &text)
{
  auto *control = new QToolButton(parent);
  control->setObjectName(QString::fromLatin1(name));
  control->setText(text);
  control->setAccessibleName(text);
  control->setAutoRaise(true);
  control->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  control->setIconSize(PaleoIcons::toolbarSize());
  control->setFocusPolicy(Qt::TabFocus);
  return control;
}

void setPresentationVisible(QDockWidget *dock, bool visible)
{
  if (auto *panel = qobject_cast<PaleoDockWidget *>(dock))
    panel->setProgrammaticVisible(visible);
  else
    dock->setVisible(visible);
}
}

class WorkspaceFocus final : public QObject
{
  Q_DECLARE_TR_FUNCTIONS(WorkspaceFocus)
public:
  WorkspaceFocus(PaleoMainWindow *window, QTabBar *tabs, QBoxLayout *layout)
    : QObject(window), m_window(window), m_tabs(tabs)
  {
    setObjectName(QStringLiteral("workspaceFocusController"));
    auto *strip = new QWidget(tabs->parentWidget());
    strip->setObjectName(QStringLiteral("workspaceDocumentStrip"));
    auto *row = new QHBoxLayout(strip);
    const int gap = PaleoTheme::tokens().spacingXs;
    row->setContentsMargins(0, 0, gap, 0);
    row->setSpacing(gap);
    tabs->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    row->addWidget(tabs, 1);
    auto *navigation = button(strip, "workspaceNavigationButton", tr("导航"));
    auto *inspector = button(strip, "workspaceInspectorButton", tr("属性"));
    m_toggle = button(strip, "workspaceFocusButton", tr("专注"));
    navigation->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionShowAllLayers.svg")));
    navigation->setToolTip(tr("显示或收起当前文档的导航栏"));
    inspector->setIcon(PaleoIcons::qgisTheme(QStringLiteral("mActionIdentify.svg")));
    inspector->setToolTip(tr("打开当前文档或所选对象的属性"));
    m_toggle->setIcon(PaleoIcons::maximize());
    m_toggle->setCheckable(true);
    m_toggle->setToolTip(tr("切换专注工作区 (F8)"));
    row->addWidget(navigation);
    row->addWidget(inspector);
    row->addWidget(m_toggle);
    layout->addWidget(strip);
    connect(navigation, &QToolButton::clicked, this, [this] {
      toggleDock(m_window->currentPage() == QLatin1String("data") ?
                 "dataListDock" : "layerTreeDock");
    });
    connect(inspector, &QToolButton::clicked, this, [this] {
      exit();
      if (auto *dock = m_window->findChild<QDockWidget *>(QStringLiteral("pagePanelDock"))) {
        dock->show();
        dock->raise();
      }
    });
    connect(m_toggle, &QToolButton::toggled, this, &WorkspaceFocus::setFocused);
    auto *shortcut = paleo::shortcuts::bindShortcut(QStringLiteral("main.workspace.focus"), window);
    connect(shortcut, &QShortcut::activated, this, [this] {
      if (m_tabs->isVisible()) m_toggle->toggle();
    });
    qApp->installEventFilter(this);
  }

  void exit() { m_toggle->setChecked(false); }

  bool suspend()
  {
    if (!m_masked) return false;
    unmask();
    return true;
  }

  void resume()
  {
    if (m_toggle->isChecked()) mask();
  }

protected:
  bool eventFilter(QObject *receiver, QEvent *event) override
  {
    if (!m_masked) return false;
    if (event->type() == QEvent::Show && receiver->parent() == m_window) {
      if (auto *dock = qobject_cast<QDockWidget *>(receiver)) {
        // A background refresh may request a panel after the original snapshot.
        // Defer the mask until Qt has finished its native show transition.
        QTimer::singleShot(0, this, [this, dock = QPointer<QDockWidget>(dock)] {
          if (!m_masked || !dock || dock->isHidden()) return;
          if (!m_visible.contains(dock)) m_visible.append(dock);
          setPresentationVisible(dock, false);
        });
      }
    }
    if (event->type() == QEvent::KeyRelease) m_escapeReserved = false;
    if (event->type() == QEvent::KeyPress) {
      auto *key = static_cast<QKeyEvent *>(event);
      if (key->key() != Qt::Key_Escape || key->modifiers() != Qt::NoModifier) return false;
      auto *widget = qobject_cast<QWidget *>(receiver);
      if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) {
        m_escapeReserved = true;
        return false;
      }
      if (!widget || widget->window() != m_window) return false;
      if (widget->property("workspaceMapToolKeys").toBool()) {
        auto *stop = m_window->findChild<QToolButton *>(QStringLiteral("stopMapToolButton"));
        if (stop && stop->isVisible() && stop->isEnabled()) m_escapeReserved = true;
      }
      // Let child controls cancel editing first. Protect a map tool's original
      // keystroke if it propagates Escape after its own synchronous deactivation.
      if (receiver == m_window && !m_escapeReserved) {
        exit();
        return true;
      }
    }
    return QObject::eventFilter(receiver, event);
  }

private:
  void toggleDock(const char *name)
  {
    exit();
    if (auto *dock = m_window->findChild<QDockWidget *>(QString::fromLatin1(name))) {
      dock->setVisible(dock->isHidden());
      if (!dock->isHidden()) dock->raise();
    }
  }

  void setFocused(bool enabled)
  {
    if (enabled) {
      m_previousFocus = QApplication::focusWidget();
      m_window->setProperty("workspaceFocused", true);
      mask();
      if (!QApplication::focusWidget() || !QApplication::focusWidget()->isVisible())
        m_tabs->setFocus(Qt::ShortcutFocusReason);
    } else {
      unmask();
      m_window->setProperty("workspaceFocused", false);
      if (m_previousFocus && m_previousFocus->isVisible())
        m_previousFocus->setFocus(Qt::OtherFocusReason);
      m_previousFocus.clear();
    }
    m_toggle->setIcon(enabled ? PaleoIcons::restore() : PaleoIcons::maximize());
    m_toggle->setText(enabled ? tr("退出专注") : tr("专注"));
    m_toggle->setAccessibleName(m_toggle->text());
  }

  void mask()
  {
    m_visible.clear();
    m_frontTabs.clear();
    const auto docks = m_window->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (auto *dock : docks)
      if (!dock->visibleRegion().isEmpty() && !m_window->tabifiedDockWidgets(dock).isEmpty())
        m_frontTabs.append(dock);
    for (auto *dock : docks) {
      if (dock->isHidden()) continue;
      m_visible.append(dock);
      setPresentationVisible(dock, false);
    }
    auto *ribbon = m_window->ribbonBar();
    m_style = ribbon->currentRibbonStyle();
    m_minimized = ribbon->isMinimumMode();
    m_showQuick = !ribbon->quickAccessBar()->isHidden();
    m_modeEnabled = ribbon->minimumModeAction()->isEnabled();
    ribbon->setRibbonStyle(SARibbonBar::RibbonStyleCompactTwoRow);
    ribbon->setMinimumMode(true);
    // The ribbon normally pops up its commands on single-click or Ctrl+1…5.
    // Keep category selection live without a popup obscuring the document.
    ribbon->ribbonStackedWidget()->setNormalMode();
    ribbon->ribbonStackedWidget()->hide();
    ribbon->setTabDoubleClickToMinimumMode(false);
    ribbon->quickAccessBar()->hide();
    ribbon->minimumModeAction()->setEnabled(false);
    m_masked = true;
  }

  void unmask()
  {
    if (!m_masked) return;
    m_masked = false;
    auto *ribbon = m_window->ribbonBar();
    ribbon->ribbonStackedWidget()->setPopupMode();
    ribbon->setTabDoubleClickToMinimumMode(true);
    ribbon->setRibbonStyle(m_style);
    ribbon->setMinimumMode(m_minimized);
    ribbon->quickAccessBar()->setVisible(m_showQuick);
    ribbon->minimumModeAction()->setEnabled(m_modeEnabled);
    for (const auto &dock : m_visible)
      if (dock) setPresentationVisible(dock, true);
    for (const auto &dock : m_frontTabs)
      if (dock && dock->isVisible()) dock->raise();
    m_visible.clear();
    m_frontTabs.clear();
  }

  PaleoMainWindow *m_window;
  QTabBar *m_tabs;
  QToolButton *m_toggle = nullptr;
  QPointer<QWidget> m_previousFocus;
  QList<QPointer<QDockWidget>> m_visible, m_frontTabs;
  SARibbonBar::RibbonStyles m_style;
  bool m_escapeReserved = false;
  bool m_masked = false, m_minimized = false, m_showQuick = true, m_modeEnabled = true;
};

namespace {
WorkspaceFocus *controller(QMainWindow *window)
{
  return static_cast<WorkspaceFocus *>(window->findChild<QObject *>(
      QStringLiteral("workspaceFocusController"), Qt::FindDirectChildrenOnly));
}
}

void installWorkspaceFocus(QMainWindow *window, QTabBar *tabs, QBoxLayout *layout)
{
  if (auto *shell = qobject_cast<PaleoMainWindow *>(window))
    new WorkspaceFocus(shell, tabs, layout);
}

void refreshWorkspaceDocumentCaptions(QTabBar *tabs)
{
  if (!tabs) return;
  const auto captions = tabs->property("workspaceDocumentCaptions").toMap();
  const auto metrics = tabs->fontMetrics();
  const int limit = metrics.horizontalAdvance(QString(18, QChar(0x4e00)));
  for (int index = 0; index < tabs->count(); ++index) {
    const auto surface = tabs->tabData(index).toMap().value(QStringLiteral("surface")).toString();
    if (surface != QLatin1String("data") && surface != QLatin1String("compose")) continue;
    const auto caption = captions.value(surface).toStringList();
    const QString fallback = surface == QLatin1String("data")
        ? PaleoMainWindow::tr("数据可视化") : PaleoMainWindow::tr("QGIS 视图");
    const QString title = caption.value(0).isEmpty() ? fallback : caption.first();
    tabs->setTabText(index, metrics.elidedText(title, Qt::ElideMiddle, limit));
    tabs->setTabToolTip(index, caption.value(1).isEmpty() ? title : title + QLatin1Char('\n') + caption.at(1));
    tabs->setAccessibleTabName(index, title);
  }
}

void setWorkspaceDocumentCaption(QTabBar *tabs, const QString &surface,
                                 const QString &title, const QString &detail)
{
  if (!tabs) return;
  auto captions = tabs->property("workspaceDocumentCaptions").toMap();
  const QStringList caption{title, detail};
  if (captions.value(surface).toStringList() == caption) return;
  captions.insert(surface, caption);
  tabs->setProperty("workspaceDocumentCaptions", captions);
  refreshWorkspaceDocumentCaptions(tabs);
}

void exitWorkspaceFocus(QMainWindow *window)
{
  if (auto *focus = controller(window)) focus->exit();
}

WorkspaceLayoutScope::WorkspaceLayoutScope(QMainWindow *window)
{
  if (auto *focus = controller(window); focus && focus->suspend())
    m_focus = focus;
}

WorkspaceLayoutScope::~WorkspaceLayoutScope()
{
  if (auto *focus = static_cast<WorkspaceFocus *>(m_focus.data())) focus->resume();
}
