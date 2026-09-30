// 层：视图
#pragma once
#include <QObject>
#include <QByteArray>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QVector>

class QDockWidget;
class QWidget;
class QLabel;
class QMainWindow;
class QMenu;
class QRubberBand;

// Owns presentation policy only. Qt retains split/tab layout and native dragging.
// Content changes never call resizeDocks: only explicit layout commands do.
class PaleoDockManager : public QObject
{
  Q_OBJECT
public:
  explicit PaleoDockManager(QMainWindow *window, const QString &settingsKey);
  void addDock(Qt::DockWidgetArea area, QDockWidget *dock);
  void removeDock(QDockWidget *dock);
  void captureDefaultLayout();
  void restoreDefaultLayout();
  void fitLayout();
  QMenu *createMenu(QWidget *parent = nullptr);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void updateDrag();
  void endDrag(bool commit);
  QMainWindow *m_window;
  QString m_settingsKey;
  QByteArray m_defaultState;
  QVector<QPointer<QDockWidget>> m_docks;
  QPointer<QDockWidget> m_dragDock;
  QPoint m_pressPosition;
  bool m_dragging = false;
  Qt::DockWidgetArea m_target = Qt::NoDockWidgetArea;
  QTimer m_dragTimer;
  QVector<QLabel *> m_guides;
  QRubberBand *m_preview;
};
