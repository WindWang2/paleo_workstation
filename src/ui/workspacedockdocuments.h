// 层：视图
#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVariantMap>

class QDockWidget;
class QStackedWidget;
class QTabBar;
class QWidget;

class WorkspaceDockDocuments final : public QObject
{
  Q_OBJECT
public:
  WorkspaceDockDocuments(QTabBar *tabs, QStackedWidget *stack);
  static void open(QWidget *window, QDockWidget *dock);
  bool activate(int index);
  void release(int index, bool restoreVisibility);
  void clear();
  QVariantMap promotedDockVisibility() const;
  void restoreDockVisibility(QWidget *window, const QVariantMap &visibility);

private:
  void promote(QDockWidget *dock);
  QString key(int index) const;
  int indexOf(const QString &name) const;
  struct Document {
    QPointer<QDockWidget> dock;
    QPointer<QWidget> content;
    QPointer<QWidget> lastFocus;
    bool wasShown = false;
    QMetaObject::Connection requestedDock;
    QMetaObject::Connection visibility;
  };
  QTabBar *m_tabs;
  QStackedWidget *m_stack;
  QHash<QString, Document> m_documents;
};
