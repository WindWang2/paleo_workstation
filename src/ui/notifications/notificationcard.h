// 层：视图
#pragma once

#include <QDateTime>
#include <QFrame>
#include <QPointer>
#include <QString>

class QLabel;
class QToolButton;
class QTimer;
class QGraphicsDropShadowEffect;
class QEnterEvent;
class QPaintEvent;

namespace paleo::services {
enum class ErrorLevel;
struct ErrorEntry;
} // namespace paleo::services

namespace paleo::ui {

/// 右下角非模态浮动通知卡片（视图层）
/// 严格遵守 DESIGN.md：8px 圆角、左侧 4px 语义色条、底部 2px 倒计时条、无编排动画
class NotificationCard : public QFrame
{
  Q_OBJECT

public:
  static constexpr int kCardWidth = 340;
  static constexpr int kMinCardHeight = 64;
  static constexpr int kMaxCardHeight = 140;
  static constexpr int kTickIntervalMs = 50;

  explicit NotificationCard(const QString &dedupKey,
                            paleo::services::ErrorLevel level,
                            const QString &title,
                            const QString &message,
                            const QString &detail = QString(),
                            int timeoutMs = 7000,
                            QWidget *parent = nullptr);
  ~NotificationCard() override;

  QString dedupKey() const { return m_dedupKey; }
  paleo::services::ErrorLevel level() const { return m_level; }
  QString title() const { return m_title; }
  QString message() const { return m_message; }
  QString detail() const { return m_detail; }
  QString domain() const { return m_domain; }
  void setDomain(const QString &domain) { m_domain = domain; }
  QDateTime timestamp() const { return m_timestamp; }
  void setTimestamp(const QDateTime &dt) { m_timestamp = dt; }

  int aggregationCount() const { return m_aggregationCount; }
  int remainingMs() const { return m_remainingMs; }
  int remainingTimeoutMs() const { return m_remainingMs; }
  int totalTimeoutMs() const { return m_totalTimeoutMs; }
  bool isHovered() const { return m_isHovered; }

  /// 当收到相同去重键时调用：累加计数、刷新文案、重置倒计时
  void updateAggregation(int count, const QString &latestMessage = QString());

  /// 重置倒计时定时器
  void resetTimeout(int newTimeoutMs = -1);
  void restartTimeout();

  /// 关闭通知
  void dismiss();

signals:
  void dismissed(NotificationCard *card);
  void actionTriggered(const QString &dedupKey);

protected:
  void paintEvent(QPaintEvent *event) override;
  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void changeEvent(QEvent *event) override;

private slots:
  void onTick();
  void onCloseClicked();
  void onCopyClicked();

private:
  void setupUi();
  void updateShadowEffect();
  void updateStyles();
  QIcon loadSeverityIcon() const;
  QColor accentColor() const;

  QString m_dedupKey;
  paleo::services::ErrorLevel m_level;
  QString m_title;
  QString m_message;
  QString m_detail;
  QString m_domain;
  QDateTime m_timestamp;
  int m_totalTimeoutMs = 7000;
  int m_remainingMs = 7000;
  int m_aggregationCount = 1;
  bool m_isHovered = false;

  QTimer *m_tickTimer = nullptr;
  QGraphicsDropShadowEffect *m_shadowEffect = nullptr;
  QLabel *m_iconLabel = nullptr;
  QLabel *m_titleLabel = nullptr;
  QLabel *m_badgeLabel = nullptr;
  QLabel *m_messageLabel = nullptr;
  QLabel *m_metaLabel = nullptr;
  QToolButton *m_closeBtn = nullptr;
  QToolButton *m_copyBtn = nullptr;
};

} // namespace paleo::ui
