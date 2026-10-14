// 层：视图
#pragma once
#include <QObject>
#include <QByteArray>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

#include <functional>

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

  // restoreState 会经 dock->setVisible 把「页面策略隐藏」读成用户意愿；
  // 意图钩子由宿主（如 PaleoMainWindow）提供：capture 取各 dock 的
  // 用户意愿快照，apply 在恢复后还原意愿并重落当前页策略。未安装时
  // 一切行为同旧版（设计器壳/预览页不带钩子）。
  struct IntentHooks {
    std::function<QVariantMap()> capture;
    std::function<void(const QVariantMap &)> apply;
  };
  void setIntentHooks(IntentHooks hooks);
  QVariantMap captureIntent() const;
  bool restoreLayout(const QByteArray &state, const QVariantMap *savedIntent);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void updateDrag();
  void endDrag(bool commit);
  void showLayoutMessage(const QString &text);
  QMainWindow *m_window;
  QString m_settingsKey;
  QByteArray m_defaultState;
  IntentHooks m_intentHooks;
  QVariantMap m_defaultIntent;
  QVector<QPointer<QDockWidget>> m_docks;
  QPointer<QDockWidget> m_dragDock;
  QPoint m_pressPosition;
  bool m_dragging = false;
  Qt::DockWidgetArea m_target = Qt::NoDockWidgetArea;
  QTimer m_dragTimer;
  QVector<QLabel *> m_guides;
  QRubberBand *m_preview;
};
