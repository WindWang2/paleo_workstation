// 层：视图
#include "notificationmanager.h"
#include "notificationcard.h"
#include "../paleotheme.h"
#include "services/errorhub.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace paleo::ui {

namespace {

class PaleoConfirmDialog : public QDialog
{
public:
  PaleoConfirmDialog(QWidget *parent, const QString &title, const QString &text,
                     const QString &confirmText, const QString &cancelText, bool destructive)
    : QDialog(parent)
  {
    setWindowTitle(title);
    setModal(true);
    setMinimumWidth(380);

    const auto &t = PaleoTheme::tokens();

    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(t.spacingMd, t.spacingMd, t.spacingMd, t.spacingMd);
    l->setSpacing(t.spacingMd);

    auto *lbl = new QLabel(text, this);
    lbl->setFont(PaleoTheme::bodyFont(t.bodyPt));
    lbl->setWordWrap(true);
    l->addWidget(lbl);

    auto *btns = new QHBoxLayout();
    btns->setContentsMargins(0, 0, 0, 0);
    btns->setSpacing(t.spacingSm);
    btns->addStretch();

    auto *cBtn = new QPushButton(cancelText, this);
    cBtn->setFont(PaleoTheme::bodyFont(t.bodyPt));
    auto *okBtn = new QPushButton(confirmText, this);
    okBtn->setFont(PaleoTheme::bodyFont(t.bodyPt));
    btns->addWidget(cBtn);
    btns->addWidget(okBtn);
    l->addLayout(btns);

    connect(cBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(okBtn, &QPushButton::clicked, this, &QDialog::accept);

    PaleoTheme::applyThemedStyleSheet(cBtn, [] {
      const auto &tok = PaleoTheme::tokens();
      return PaleoTheme::metricStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; border: 1px solid %1; color: %2; "
        "border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.md}px; } "
        "QPushButton:hover { background: %3; }"))
        .arg(tok.border.name().toUpper(),
             tok.text.name().toUpper(),
             tok.surfaceAltRaised.name().toUpper());
    });

    PaleoTheme::applyThemedStyleSheet(okBtn, [destructive] {
      const auto &tok = PaleoTheme::tokens();
      if (destructive) {
        return PaleoTheme::metricStyleSheet(QStringLiteral(
          "QPushButton { background-color: %1; color: %2; border: none; "
          "border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.md}px; } "
          "QPushButton:hover { background-color: %3; }"))
          .arg(tok.error.name().toUpper(),
               tok.onPrimary.name().toUpper(),
               tok.error.darker(110).name().toUpper());
      } else {
        return PaleoTheme::metricStyleSheet(QStringLiteral(
          "QPushButton { background-color: %1; color: %2; border: none; "
          "border-radius: {rounded.sm}px; padding: {spacing.xs}px {spacing.md}px; } "
          "QPushButton:hover { background-color: %3; }"))
          .arg(tok.primary.name().toUpper(),
               tok.onPrimary.name().toUpper(),
               tok.primaryHover.name().toUpper());
      }
    });
  }
};

} // namespace

NotificationManager *NotificationManager::s_instance = nullptr;
bool NotificationManager::s_offscreenAutoAnswer = true;
NotificationManager::ConfirmHook NotificationManager::s_confirmHook = nullptr;

NotificationManager *NotificationManager::instance()
{
  return s_instance;
}

void NotificationManager::setInstance(NotificationManager *mgr)
{
  s_instance = mgr;
}

void NotificationManager::setOffscreenAutoAnswer(bool answer)
{
  s_offscreenAutoAnswer = answer;
}

bool NotificationManager::offscreenAutoAnswer()
{
  return s_offscreenAutoAnswer;
}

void NotificationManager::setConfirmHookForTesting(ConfirmHook hook)
{
  s_confirmHook = std::move(hook);
}

NotificationManager::NotificationManager(QWidget *mainWindowParent,
                                         paleo::services::ErrorHub *hub)
  : QObject(mainWindowParent)
  , m_mainWindow(mainWindowParent)
  , m_hub(hub)
{
  if (!s_instance) {
    s_instance = this;
  }

  if (m_mainWindow) {
    m_mainWindow->installEventFilter(this);
  }

  if (!m_hub) {
    m_hub = paleo::services::ErrorHub::instance();
  }

  if (m_hub) {
    connect(m_hub, &paleo::services::ErrorHub::errorRaised,
            this, &NotificationManager::onErrorRaised);
    connect(m_hub, &paleo::services::ErrorHub::errorAggregated,
            this, &NotificationManager::onErrorAggregated);
  }
}

NotificationManager::~NotificationManager()
{
  if (s_instance == this) {
    s_instance = nullptr;
  }
  clearAll();
  if (m_overlayContainer) {
    m_overlayContainer->deleteLater();
    m_overlayContainer = nullptr;
  }
}

void NotificationManager::setParentWindow(QWidget *mainWindow)
{
  if (m_mainWindow == mainWindow)
    return;

  if (m_mainWindow) {
    m_mainWindow->removeEventFilter(this);
  }
  m_mainWindow = mainWindow;
  if (m_mainWindow) {
    m_mainWindow->installEventFilter(this);
    if (m_overlayContainer) {
      m_overlayContainer->setParent(m_mainWindow);
      repositionOverlay();
    }
  }
}

QWidget *NotificationManager::overlayContainer()
{
  ensureOverlayContainer();
  return m_overlayContainer;
}

int NotificationManager::defaultTimeoutForLevel(paleo::services::ErrorLevel level) const
{
  switch (level) {
    case paleo::services::ErrorLevel::Info:
      return kDefaultTimeoutInfoMs;
    case paleo::services::ErrorLevel::Warning:
      return kDefaultTimeoutWarningMs;
    case paleo::services::ErrorLevel::Error:
      return kDefaultTimeoutErrorMs;
    case paleo::services::ErrorLevel::Critical:
    default:
      return kDefaultTimeoutCriticalMs;
  }
}

void NotificationManager::ensureOverlayContainer()
{
  if (!m_overlayContainer && m_mainWindow) {
    m_overlayContainer = new QWidget(m_mainWindow);
    m_overlayContainer->setObjectName(QStringLiteral("notificationOverlayContainer"));
    m_overlayContainer->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_overlayContainer->setFixedWidth(360);
    const auto &t = PaleoTheme::tokens();
    m_cardLayout = new QVBoxLayout(m_overlayContainer);
    m_cardLayout->setContentsMargins(0, 0, 0, 0);
    m_cardLayout->setSpacing(t.spacingSm);
    m_overlayContainer->hide();
  }
}

void NotificationManager::repositionOverlay()
{
  if (!m_mainWindow || !m_overlayContainer)
    return;

  m_overlayContainer->adjustSize();
  const int x = m_mainWindow->width() - 360 - 16;
  const int y = m_mainWindow->height() - m_overlayContainer->height() - 36;
  m_overlayContainer->move(qMax(0, x), qMax(0, y));
}

void NotificationManager::showNotification(paleo::services::ErrorLevel level,
                                           const QString &title,
                                           const QString &message,
                                           const QString &detail,
                                           const QString &dedupKey,
                                           int timeoutMs)
{
  const int actualTimeout = (timeoutMs > 0) ? timeoutMs : defaultTimeoutForLevel(level);

  // Check deduplication in active cards
  if (!dedupKey.isEmpty()) {
    for (auto *card : m_activeCards) {
      if (card && card->dedupKey() == dedupKey) {
        card->updateAggregation(card->aggregationCount() + 1, message);
        return;
      }
    }
    for (auto &item : m_pendingQueue) {
      if (item.dedupKey == dedupKey) {
        item.aggregationCount++;
        item.message = message;
        return;
      }
    }
  }

  if (m_activeCards.size() < kMaxVisibleCards) {
    ensureOverlayContainer();
    QWidget *cardParent = m_overlayContainer ? m_overlayContainer.data() : m_mainWindow.data();
    auto *card = new NotificationCard(dedupKey, level, title, message, detail, actualTimeout, cardParent);
    connect(card, &NotificationCard::dismissed, this, &NotificationManager::onCardDismissed);
    m_activeCards.append(card);

    if (m_overlayContainer && m_cardLayout) {
      m_cardLayout->addWidget(card);
      repositionOverlay();
      m_overlayContainer->show();
      m_overlayContainer->raise();
    } else {
      card->show();
    }
  } else {
    if (m_pendingQueue.size() >= kMaxPendingQueue) {
      m_pendingQueue.dequeue(); // FIFO eviction
    }
    m_pendingQueue.enqueue({level, title, message, detail, dedupKey, actualTimeout, 1});
  }
}

void NotificationManager::onCardDismissed(NotificationCard *card)
{
  if (!card)
    return;

  m_activeCards.removeOne(card);
  if (m_cardLayout) {
    m_cardLayout->removeWidget(card);
  }
  card->deleteLater();
  processQueue();
}

void NotificationManager::processQueue()
{
  while (m_activeCards.size() < kMaxVisibleCards && !m_pendingQueue.isEmpty()) {
    const auto item = m_pendingQueue.dequeue();
    ensureOverlayContainer();
    QWidget *cardParent = m_overlayContainer ? m_overlayContainer.data() : m_mainWindow.data();
    auto *card = new NotificationCard(item.dedupKey, item.level, item.title, item.message,
                                      item.detail, item.timeoutMs, cardParent);
    if (item.aggregationCount > 1) {
      card->updateAggregation(item.aggregationCount, item.message);
    }
    connect(card, &NotificationCard::dismissed, this, &NotificationManager::onCardDismissed);
    m_activeCards.append(card);

    if (m_overlayContainer && m_cardLayout) {
      m_cardLayout->addWidget(card);
    } else {
      card->show();
    }
  }

  if (m_activeCards.isEmpty()) {
    if (m_overlayContainer) {
      m_overlayContainer->hide();
    }
  } else {
    repositionOverlay();
    if (m_overlayContainer) {
      m_overlayContainer->show();
      m_overlayContainer->raise();
    }
  }
}

void NotificationManager::clearAll()
{
  m_pendingQueue.clear();
  for (auto *card : m_activeCards) {
    if (card) {
      if (m_cardLayout) {
        m_cardLayout->removeWidget(card);
      }
      card->deleteLater();
    }
  }
  m_activeCards.clear();
  if (m_overlayContainer) {
    m_overlayContainer->hide();
  }
}

bool NotificationManager::shouldSuppressModal(const QString &dedupKey) const
{
  if (dedupKey.isEmpty() || !m_modalPopupTimestamps.contains(dedupKey))
    return false;
  const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_modalPopupTimestamps.value(dedupKey);
  return (elapsed >= 0 && elapsed < m_modalDedupWindowMs);
}

void NotificationManager::recordModalShown(const QString &dedupKey)
{
  if (!dedupKey.isEmpty()) {
    m_modalPopupTimestamps.insert(dedupKey, QDateTime::currentMSecsSinceEpoch());
  }
}

void NotificationManager::onErrorRaised(const paleo::services::ErrorEntry &entry)
{
  if (entry.isModal) {
    if (shouldSuppressModal(entry.deduplicationKey)) {
      showNotification(entry.level,
                       tr("[聚合抑制] %1").arg(entry.domain),
                       entry.message,
                       entry.details,
                       entry.deduplicationKey);
      return;
    }

    recordModalShown(entry.deduplicationKey);
    if (s_confirmHook) {
      s_confirmHook(entry.domain, entry.message);
      return;
    }
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
      return;
    }
    PaleoConfirmDialog dlg(m_mainWindow, entry.domain, entry.message, tr("确定"), tr("取消"), false);
    dlg.exec();
  } else {
    showNotification(entry.level, entry.domain, entry.message, entry.details,
                     entry.deduplicationKey);
  }
}

void NotificationManager::onErrorAggregated(const paleo::services::ErrorEntry &entry)
{
  for (auto *card : m_activeCards) {
    if (card && card->dedupKey() == entry.deduplicationKey) {
      card->updateAggregation(entry.aggregationCount, entry.message);
      return;
    }
  }

  for (auto &item : m_pendingQueue) {
    if (item.dedupKey == entry.deduplicationKey) {
      item.message = entry.message;
      item.aggregationCount = entry.aggregationCount;
      return;
    }
  }

  if (entry.isModal && shouldSuppressModal(entry.deduplicationKey)) {
    showNotification(entry.level,
                     tr("[聚合抑制] %1").arg(entry.domain),
                     entry.message,
                     entry.details,
                     entry.deduplicationKey);
  }
}

bool NotificationManager::eventFilter(QObject *watched, QEvent *event)
{
  if (watched == m_mainWindow && (event->type() == QEvent::Resize || event->type() == QEvent::Move)) {
    repositionOverlay();
  }
  return QObject::eventFilter(watched, event);
}

// ---- Static Convenience Helpers ----

void NotificationManager::showInfo(QWidget *parent, const QString &title,
                                   const QString &message, const QString &detail)
{
  if (auto *mgr = instance()) {
    mgr->showNotification(paleo::services::ErrorLevel::Info, title, message, detail);
  } else if (parent) {
    if (auto *mgr = parent->findChild<NotificationManager *>()) {
      mgr->showNotification(paleo::services::ErrorLevel::Info, title, message, detail);
    } else if (auto *hub = paleo::services::ErrorHub::instance()) {
      hub->reportInfo(paleo::services::ErrorDomain::General, message.isEmpty() ? title : QStringLiteral("%1: %2").arg(title, message), detail);
    }
  } else if (auto *hub = paleo::services::ErrorHub::instance()) {
    hub->reportInfo(paleo::services::ErrorDomain::General, message.isEmpty() ? title : QStringLiteral("%1: %2").arg(title, message), detail);
  }
}

void NotificationManager::showWarning(QWidget *parent, const QString &title,
                                      const QString &message, const QString &detail)
{
  if (auto *mgr = instance()) {
    mgr->showNotification(paleo::services::ErrorLevel::Warning, title, message, detail);
  } else if (parent) {
    if (auto *mgr = parent->findChild<NotificationManager *>()) {
      mgr->showNotification(paleo::services::ErrorLevel::Warning, title, message, detail);
    } else if (auto *hub = paleo::services::ErrorHub::instance()) {
      hub->reportWarning(paleo::services::ErrorDomain::General, message.isEmpty() ? title : QStringLiteral("%1: %2").arg(title, message), detail);
    }
  } else if (auto *hub = paleo::services::ErrorHub::instance()) {
    hub->reportWarning(paleo::services::ErrorDomain::General, message.isEmpty() ? title : QStringLiteral("%1: %2").arg(title, message), detail);
  }
}

void NotificationManager::showError(QWidget *parent, const QString &title,
                                    const QString &message, const QString &detail)
{
  if (auto *mgr = instance()) {
    mgr->showNotification(paleo::services::ErrorLevel::Error, title, message, detail);
  } else if (parent) {
    if (auto *mgr = parent->findChild<NotificationManager *>()) {
      mgr->showNotification(paleo::services::ErrorLevel::Error, title, message, detail);
    } else if (auto *hub = paleo::services::ErrorHub::instance()) {
      hub->reportError(paleo::services::ErrorDomain::General, message.isEmpty() ? title : QStringLiteral("%1: %2").arg(title, message), detail);
    }
  } else if (auto *hub = paleo::services::ErrorHub::instance()) {
    hub->reportError(paleo::services::ErrorDomain::General, message.isEmpty() ? title : QStringLiteral("%1: %2").arg(title, message), detail);
  }
}

bool NotificationManager::confirmOkCancel(QWidget *parent, const QString &title, const QString &text)
{
  if (s_confirmHook) {
    if (auto res = s_confirmHook(title, text))
      return *res;
  }
  if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
    return s_offscreenAutoAnswer;
  }
  PaleoConfirmDialog dlg(parent, title, text, QObject::tr("确定"), QObject::tr("取消"), false);
  return dlg.exec() == QDialog::Accepted;
}

bool NotificationManager::confirmYesNo(QWidget *parent, const QString &title, const QString &text)
{
  if (s_confirmHook) {
    if (auto res = s_confirmHook(title, text))
      return *res;
  }
  if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
    return s_offscreenAutoAnswer;
  }
  PaleoConfirmDialog dlg(parent, title, text, QObject::tr("是"), QObject::tr("否"), false);
  return dlg.exec() == QDialog::Accepted;
}

bool NotificationManager::confirmDestructive(QWidget *parent, const QString &title, const QString &text)
{
  if (s_confirmHook) {
    if (auto res = s_confirmHook(title, text))
      return *res;
  }
  if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
    return s_offscreenAutoAnswer;
  }
  PaleoConfirmDialog dlg(parent, title, text, QObject::tr("确定删除"), QObject::tr("取消"), true);
  return dlg.exec() == QDialog::Accepted;
}

} // namespace paleo::ui
