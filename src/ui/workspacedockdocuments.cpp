// 层：视图
#include "workspacedockdocuments.h"

#include "paleomainwindow.h"
#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QVariantMap>

namespace {
void showDock(QDockWidget *dock, bool visible)
{
  if (auto *managed = qobject_cast<PaleoDockWidget *>(dock))
    managed->setProgrammaticVisible(visible);
  else
    dock->setVisible(visible);
}
}

WorkspaceDockDocuments::WorkspaceDockDocuments(QTabBar *tabs, QStackedWidget *stack)
    : QObject(tabs), m_tabs(tabs), m_stack(stack)
{
  connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
    auto doc = m_documents.find(key(m_tabs->currentIndex()));
    if (doc != m_documents.end() && now && doc->content &&
        (doc->content == now || doc->content->isAncestorOf(now)))
      doc->lastFocus = now;
  });
}

QString WorkspaceDockDocuments::key(int index) const
{
  return m_tabs->tabData(index).toMap().value(QStringLiteral("dockDocument")).toString();
}

int WorkspaceDockDocuments::indexOf(const QString &name) const
{
  for (int index = 0; index < m_tabs->count(); ++index)
    if (key(index) == name) return index;
  return -1;
}

void WorkspaceDockDocuments::open(QWidget *window, QDockWidget *dock)
{
  if (!dock) return;
  auto *documents = window->findChild<WorkspaceDockDocuments *>();
  if (documents && documents->m_tabs->count() > 0) {
    documents->promote(dock);
  } else {
    dock->show();
    dock->raise();
  }
}

void WorkspaceDockDocuments::promote(QDockWidget *dock)
{
  const QString name = dock->objectName();
  int index = indexOf(name);
  if (index < 0) {
    QWidget *content = dock->widget();
    if (!content) return;
    Document document{dock, content, content->focusWidget(), !dock->isHidden(), {}, {}};
    dock->setProperty("workspaceCentralDocument", true);
    showDock(dock, false);
    // Keep the dock, its saved position and the view's undo/selection model alive.
    m_stack->addWidget(content);
    document.requestedDock = connect(dock->toggleViewAction(), &QAction::triggered, this, [this, dock, name](bool shown) {
      const int tab = indexOf(name);
      if (!shown || tab < 0) return;
      release(tab, false);
      m_tabs->removeTab(tab);
      // Closing the active tab can apply the fallback page's dock policy.
      // The user's explicit request to open this panel takes precedence.
      showDock(dock, true);
      dock->raise();
    });
    // QMainWindow::restoreState() can show the empty dock independently of user
    // intent. Let an explicit menu action return the view before hiding its host.
    document.visibility = connect(dock, &QDockWidget::visibilityChanged, this, [this, name](bool shown) {
      if (shown) QTimer::singleShot(0, this, [this, name] {
        const auto doc = m_documents.value(name);
        if (doc.dock && !doc.dock->isHidden()) showDock(doc.dock, false);
      });
    });
    m_documents.insert(name, document);
    QSignalBlocker block(m_tabs);
    index = m_tabs->addTab(dock->windowTitle());
    m_tabs->setTabData(index, QVariantMap{{QStringLiteral("dockDocument"), name}});
    m_tabs->setTabToolTip(index, dock->windowTitle());
  }
  m_tabs->setCurrentIndex(index);
  activate(index);
}

bool WorkspaceDockDocuments::activate(int index)
{
  const auto document = m_documents.value(key(index));
  if (!document.content) return false;
  m_stack->setCurrentWidget(document.content);
  QWidget *focus = document.lastFocus;
  if (!focus || focus == document.content || !document.content->isAncestorOf(focus) || !focus->isVisible()) {
    auto *scroll = qobject_cast<QScrollArea *>(document.content.data());
    focus = scroll && scroll->widget() ? scroll->widget() : document.content.data();
  }
  focus->setFocus(Qt::OtherFocusReason);
  return true;
}

void WorkspaceDockDocuments::release(int index, bool restoreVisibility)
{
  const auto document = m_documents.take(key(index));
  if (!document.content || !document.dock) return;
  disconnect(document.requestedDock);
  disconnect(document.visibility);
  document.dock->setProperty("workspaceCentralDocument", false);
  m_stack->removeWidget(document.content);
  document.dock->setWidget(document.content);
  document.content->show();
  if (restoreVisibility && document.wasShown) showDock(document.dock, true);
}

void WorkspaceDockDocuments::clear()
{
  for (int index = 0; index < m_tabs->count(); ++index)
    release(index, false);
}

QVariantMap WorkspaceDockDocuments::promotedDockVisibility() const
{
  QVariantMap visibility;
  for (auto doc = m_documents.cbegin(); doc != m_documents.cend(); ++doc) {
    // Managed docks already have a persisted userWantsVisible independent of Qt's hidden bit.
    if (doc->dock && !qobject_cast<PaleoDockWidget *>(doc->dock.data()))
      visibility.insert(doc.key(), doc->wasShown);
  }
  return visibility;
}

void WorkspaceDockDocuments::restoreDockVisibility(QWidget *window, const QVariantMap &visibility)
{
  // restoreState supplies the visibility of ordinary, unpromoted docks in older layouts.
  // During promotion the saved override restores the original preference instead of the empty host.
  for (auto it = m_documents.begin(); it != m_documents.end(); ++it) {
    if (!it->dock || qobject_cast<PaleoDockWidget *>(it->dock.data())) continue;
    it->wasShown = visibility.value(it.key(), !it->dock->isHidden()).toBool();
    showDock(it->dock, false);
  }
  for (auto it = visibility.cbegin(); it != visibility.cend(); ++it) {
    if (m_documents.contains(it.key())) continue;
    if (auto *dock = window->findChild<QDockWidget *>(it.key(), Qt::FindDirectChildrenOnly))
      showDock(dock, it.value().toBool());
  }
}
