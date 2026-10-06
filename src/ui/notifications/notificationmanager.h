// 层：视图
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <QWidget>
#include <functional>
#include <optional>

class QVBoxLayout;

namespace paleo::services {
enum class ErrorLevel;
struct ErrorEntry;
class ErrorHub;
} // namespace paleo::services

namespace paleo::ui {

class NotificationCard;

/// 通知与错误呈现管理器（视图层）
/// 负责右下角堆叠容器管理（最多 5 张并发）、排队管理（最多 50 条 FIFO）、
/// 模态 60s 去重抑制、统一确认弹窗辅助函数（0 QMessageBox Token 消耗）及 ErrorHub 信号订阅。
class NotificationManager : public QObject
{
  Q_OBJECT

public:
  static constexpr int kMaxVisibleCards = 5;
  static constexpr int kMaxPendingQueue = 50;
  static constexpr qint64 kDefaultModalDedupWindowMs = 60000; // 60s
  static constexpr int kDefaultTimeoutInfoMs = 4000;
  static constexpr int kDefaultTimeoutWarningMs = 7000;
  static constexpr int kDefaultTimeoutErrorMs = 10000;
  static constexpr int kDefaultTimeoutCriticalMs = 10000;

  explicit NotificationManager(QWidget *mainWindowParent = nullptr,
                               paleo::services::ErrorHub *hub = nullptr);
  ~NotificationManager() override;

  static NotificationManager *instance();
  static void setInstance(NotificationManager *mgr);

  // 宿主绑定
  void setParentWindow(QWidget *mainWindow);
  QWidget *parentWindow() const { return m_mainWindow; }

  // 状态检查
  QWidget *overlayContainer();
  int activeCardCount() const { return m_activeCards.size(); }
  int pendingQueueCount() const { return m_pendingQueue.size(); }
  QList<NotificationCard *> activeCards() const { return m_activeCards; }

  // 核心非模态通知派发
  void showNotification(paleo::services::ErrorLevel level,
                        const QString &title,
                        const QString &message,
                        const QString &detail = QString(),
                        const QString &dedupKey = QString(),
                        int timeoutMs = -1);

  // 静态便捷展示接口
  static void showInfo(QWidget *parent, const QString &title, const QString &message, const QString &detail = QString());
  static void showWarning(QWidget *parent, const QString &title, const QString &message, const QString &detail = QString());
  static void showError(QWidget *parent, const QString &title, const QString &message, const QString &detail = QString());

  // 统一模态确认辅助函数 (严格 0 QMessageBox Token 消耗)
  static bool confirmOkCancel(QWidget *parent, const QString &title, const QString &text);
  static bool confirmYesNo(QWidget *parent, const QString &title, const QString &text);
  static bool confirmDestructive(QWidget *parent, const QString &title, const QString &text);

  // 离线无头测试应答注入探针
  static void setOffscreenAutoAnswer(bool answer);
  static bool offscreenAutoAnswer();

  // 测试 Hook
  using ConfirmHook = std::function<std::optional<bool>(const QString &title, const QString &text)>;
  static void setConfirmHookForTesting(ConfirmHook hook);

  // 60s 模态去重抑制策略
  bool shouldSuppressModal(const QString &dedupKey) const;
  void recordModalShown(const QString &dedupKey);
  void setModalDedupWindowMs(qint64 ms) { m_modalDedupWindowMs = ms; }
  qint64 modalDedupWindowMs() const { return m_modalDedupWindowMs; }

  // 清除全部卡片与排队
  void clearAll();

public slots:
  void onErrorRaised(const paleo::services::ErrorEntry &entry);
  void onErrorAggregated(const paleo::services::ErrorEntry &entry);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
  void onCardDismissed(NotificationCard *card);

private:
  struct PendingNotification {
    paleo::services::ErrorLevel level;
    QString title;
    QString message;
    QString detail;
    QString dedupKey;
    int timeoutMs = -1;
    int aggregationCount = 1;
  };

  void ensureOverlayContainer();
  void repositionOverlay();
  void processQueue();
  int defaultTimeoutForLevel(paleo::services::ErrorLevel level) const;

  static NotificationManager *s_instance;
  static bool s_offscreenAutoAnswer;
  static ConfirmHook s_confirmHook;

  QPointer<QWidget> m_mainWindow;
  QPointer<QWidget> m_overlayContainer;
  QVBoxLayout *m_cardLayout = nullptr;

  QList<NotificationCard *> m_activeCards;
  QQueue<PendingNotification> m_pendingQueue;
  QHash<QString, qint64> m_modalPopupTimestamps;
  qint64 m_modalDedupWindowMs = kDefaultModalDedupWindowMs;
  paleo::services::ErrorHub *m_hub = nullptr;
};

} // namespace paleo::ui
