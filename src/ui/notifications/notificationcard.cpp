// 层：视图
#include "notificationcard.h"

#include "../paleoicons.h"
#include "../paleotheme.h"
#include "services/errorhub.h"

#include <QClipboard>
#include <QEnterEvent>
#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace paleo::ui {

NotificationCard::NotificationCard(const QString &dedupKey,
                                   paleo::services::ErrorLevel level,
                                   const QString &title,
                                   const QString &message,
                                   const QString &detail,
                                   int timeoutMs,
                                   QWidget *parent)
  : QFrame(parent)
  , m_dedupKey(dedupKey)
  , m_level(level)
  , m_title(title)
  , m_message(message)
  , m_detail(detail)
  , m_timestamp(QDateTime::currentDateTime())
  , m_totalTimeoutMs(timeoutMs)
  , m_remainingMs(timeoutMs)
{
  setObjectName(QStringLiteral("notificationCard"));
  setFixedWidth(kCardWidth);
  setMinimumHeight(kMinCardHeight);
  setMaximumHeight(kMaxCardHeight);
  setAttribute(Qt::WA_Hover, true);

  setupUi();
  updateShadowEffect();

  if (m_totalTimeoutMs > 0) {
    m_tickTimer = new QTimer(this);
    m_tickTimer->setInterval(kTickIntervalMs);
    connect(m_tickTimer, &QTimer::timeout, this, &NotificationCard::onTick);
    QTimer::singleShot(0, this, [this] {
      if (m_tickTimer && !m_isHovered && m_totalTimeoutMs > 0 && !m_tickTimer->isActive()) {
        m_tickTimer->start();
      }
    });
  }
}

NotificationCard::~NotificationCard()
{
  if (m_tickTimer) {
    m_tickTimer->stop();
  }
}

QColor NotificationCard::accentColor() const
{
  const auto &t = PaleoTheme::tokens();
  switch (m_level) {
    case paleo::services::ErrorLevel::Info:
      return (PaleoTheme::currentTheme() == PaleoTheme::Theme::Dark) ? t.primaryText : t.primary;
    case paleo::services::ErrorLevel::Warning:
      return t.warning;
    case paleo::services::ErrorLevel::Error:
    case paleo::services::ErrorLevel::Critical:
    default:
      return t.error;
  }
}

QIcon NotificationCard::loadSeverityIcon() const
{
  QIcon icon;
  switch (m_level) {
    case paleo::services::ErrorLevel::Info:
      icon = PaleoIcons::qgisTheme(QStringLiteral("mMessageLogRead.svg"));
      if (icon.isNull())
        icon = PaleoIcons::qgisTheme(QStringLiteral("mIconInfo.svg"));
      break;
    case paleo::services::ErrorLevel::Warning:
      icon = PaleoIcons::qgisTheme(QStringLiteral("mIconWarning.svg"));
      break;
    case paleo::services::ErrorLevel::Error:
    case paleo::services::ErrorLevel::Critical:
    default:
      icon = PaleoIcons::qgisTheme(QStringLiteral("mIconCritical.svg"));
      break;
  }
  if (icon.isNull() && style()) {
    switch (m_level) {
      case paleo::services::ErrorLevel::Info:
        icon = style()->standardIcon(QStyle::SP_MessageBoxInformation);
        break;
      case paleo::services::ErrorLevel::Warning:
        icon = style()->standardIcon(QStyle::SP_MessageBoxWarning);
        break;
      case paleo::services::ErrorLevel::Error:
      case paleo::services::ErrorLevel::Critical:
      default:
        icon = style()->standardIcon(QStyle::SP_MessageBoxCritical);
        break;
    }
  }
  return icon;
}

void NotificationCard::setupUi()
{
  const auto &t = PaleoTheme::tokens();

  auto *mainLayout = new QHBoxLayout(this);
  mainLayout->setContentsMargins(t.spacingMd, t.spacingSm, t.spacingSm, t.spacingSm);
  mainLayout->setSpacing(t.spacingSm);

  // 1. Severity Icon
  m_iconLabel = new QLabel(this);
  m_iconLabel->setObjectName(QStringLiteral("cardIconLabel"));
  m_iconLabel->setFixedSize(18, 18);
  m_iconLabel->setAlignment(Qt::AlignCenter);
  QIcon icon = loadSeverityIcon();
  if (!icon.isNull()) {
    m_iconLabel->setPixmap(icon.pixmap(18, 18));
  } else {
    m_iconLabel->setText(m_level == paleo::services::ErrorLevel::Info ? QStringLiteral("ℹ")
                       : m_level == paleo::services::ErrorLevel::Warning ? QStringLiteral("⚠")
                       : QStringLiteral("✖"));
  }
  mainLayout->addWidget(m_iconLabel, 0, Qt::AlignTop);

  // 2. Center Content Column
  auto *contentLayout = new QVBoxLayout();
  contentLayout->setContentsMargins(0, 0, 0, 0);
  contentLayout->setSpacing(t.spacingXs);

  // Header Row: Title + Badge + Stretch + Copy + Close
  auto *headerLayout = new QHBoxLayout();
  headerLayout->setContentsMargins(0, 0, 0, 0);
  headerLayout->setSpacing(t.spacingXs);

  m_titleLabel = new QLabel(m_title, this);
  m_titleLabel->setObjectName(QStringLiteral("cardTitleLabel"));
  QFont titleFont = PaleoTheme::bodyFont(t.bodyPt);
  titleFont.setBold(true);
  m_titleLabel->setFont(titleFont);
  headerLayout->addWidget(m_titleLabel);

  // Aggregation Badge (capsule)
  PaleoTheme::CapsuleKind capKind =
      m_level == paleo::services::ErrorLevel::Warning ? PaleoTheme::CapsuleKind::Warning :
      m_level == paleo::services::ErrorLevel::Info    ? PaleoTheme::CapsuleKind::Neutral :
                                                        PaleoTheme::CapsuleKind::Error;
  m_badgeLabel = PaleoTheme::capsuleLabel(QStringLiteral("×%1").arg(m_aggregationCount), capKind, this);
  m_badgeLabel->setObjectName(QStringLiteral("cardBadgeLabel"));
  m_badgeLabel->setVisible(m_aggregationCount > 1);
  headerLayout->addWidget(m_badgeLabel);

  headerLayout->addStretch(1);

  if (!m_detail.isEmpty()) {
    m_copyBtn = new QToolButton(this);
    m_copyBtn->setObjectName(QStringLiteral("cardCopyButton"));
    m_copyBtn->setText(tr("复制"));
    m_copyBtn->setToolTip(tr("复制错误与详情"));
    connect(m_copyBtn, &QToolButton::clicked, this, &NotificationCard::onCopyClicked);
    headerLayout->addWidget(m_copyBtn);
  }

  m_closeBtn = new QToolButton(this);
  m_closeBtn->setObjectName(QStringLiteral("cardCloseButton"));
  if (style()) {
    m_closeBtn->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
  } else {
    m_closeBtn->setText(QStringLiteral("✕"));
  }
  m_closeBtn->setToolTip(tr("关闭通知"));
  connect(m_closeBtn, &QToolButton::clicked, this, &NotificationCard::onCloseClicked);
  headerLayout->addWidget(m_closeBtn);

  contentLayout->addLayout(headerLayout);

  // Message Row
  m_messageLabel = new QLabel(m_message, this);
  m_messageLabel->setObjectName(QStringLiteral("cardMessageLabel"));
  m_messageLabel->setFont(PaleoTheme::bodyFont(t.bodyPt));
  m_messageLabel->setWordWrap(true);
  m_messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  contentLayout->addWidget(m_messageLabel);

  // Meta Row (Time + Domain)
  QString timeStr = m_timestamp.isValid() ? m_timestamp.toString(QStringLiteral("hh:mm:ss"))
                                          : QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss"));
  QString metaText = m_domain.isEmpty() ? timeStr : QStringLiteral("%1 · [%2]").arg(timeStr, m_domain);
  m_metaLabel = new QLabel(metaText, this);
  m_metaLabel->setObjectName(QStringLiteral("cardMetaLabel"));
  m_metaLabel->setFont(PaleoTheme::monoFont(t.labelPt));
  contentLayout->addWidget(m_metaLabel);

  mainLayout->addLayout(contentLayout, 1);

  updateStyles();
}

void NotificationCard::updateStyles()
{
  PaleoTheme::applyThemedStyleSheet(m_closeBtn, [] {
    const auto &t = PaleoTheme::tokens();
    return PaleoTheme::metricStyleSheet(QStringLiteral(
               "QToolButton { border: none; background: transparent; color: %1;"
               " border-radius: {rounded.sm}px; padding: {spacing.xs}px; width: {spacing.md}px; height: {spacing.md}px; }"
               "QToolButton:hover { background: %2; }"
               "QToolButton:pressed { background: %3; }"))
        .arg(t.textMuted.name().toUpper(),
             t.surfaceAltRaised.name().toUpper(),
             t.border.name().toUpper());
  });

  if (m_copyBtn) {
    PaleoTheme::applyThemedStyleSheet(m_copyBtn, [] {
      const auto &t = PaleoTheme::tokens();
      return PaleoTheme::metricStyleSheet(QStringLiteral(
                 "QToolButton { border: 1px solid %3; background: transparent; color: %1;"
                 " border-radius: {rounded.sm}px; padding: 0 {spacing.xs}px; font-size: {typography.label}pt; }"
                 "QToolButton:hover { background: %2; }"))
          .arg(t.textMuted.name().toUpper(),
               t.surfaceAltRaised.name().toUpper(),
               t.border.name().toUpper());
    });
  }

  PaleoTheme::applyThemedStyleSheet(m_titleLabel, [] {
    return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().text.name().toUpper());
  });

  PaleoTheme::applyThemedStyleSheet(m_messageLabel, [] {
    return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().textMuted.name().toUpper());
  });

  PaleoTheme::applyThemedStyleSheet(m_metaLabel, [] {
    return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().textMuted.name().toUpper());
  });
}

void NotificationCard::updateShadowEffect()
{
  if (!m_shadowEffect) {
    m_shadowEffect = new QGraphicsDropShadowEffect(this);
    m_shadowEffect->setOffset(0, 3);
    m_shadowEffect->setBlurRadius(8);
    setGraphicsEffect(m_shadowEffect);
  }
  const bool dark = (PaleoTheme::currentTheme() == PaleoTheme::Theme::Dark);
  QColor shadowColor = PaleoTheme::tokens().text;
  shadowColor.setAlpha(dark ? 82 : 41);
  m_shadowEffect->setColor(shadowColor);
}

void NotificationCard::paintEvent(QPaintEvent *)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const auto &t = PaleoTheme::tokens();

  QRectF bounds = rect();
  bounds.adjust(0.5, 0.5, -0.5, -0.5);

  QPainterPath clipPath;
  clipPath.addRoundedRect(bounds, t.radiusMd, t.radiusMd);

  // 1. Background
  painter.fillPath(clipPath, t.surface);

  // 2. Outer Border (1px)
  painter.strokePath(clipPath, QPen(t.border, 1.0));

  painter.save();
  painter.setClipPath(clipPath);

  // 3. Left Accent Stripe (4px)
  const QColor accent = accentColor();
  painter.fillRect(QRectF(0, 0, 4.0, height()), accent);

  // 4. Embedded Bottom Countdown Progress Bar (2px)
  if (m_totalTimeoutMs > 0 && m_remainingMs > 0) {
    const qreal progress = qBound(0.0, static_cast<qreal>(m_remainingMs) / m_totalTimeoutMs, 1.0);
    const qreal barWidth = width() * progress;
    painter.fillRect(QRectF(0, height() - 2.0, barWidth, 2.0), accent);
  }

  painter.restore();
}

void NotificationCard::enterEvent(QEnterEvent *event)
{
  QFrame::enterEvent(event);
  m_isHovered = true;
  if (m_tickTimer && m_tickTimer->isActive()) {
    m_tickTimer->stop();
  }
}

void NotificationCard::leaveEvent(QEvent *event)
{
  QFrame::leaveEvent(event);
  m_isHovered = false;
  if (m_tickTimer && m_remainingMs > 0 && !m_tickTimer->isActive()) {
    m_tickTimer->start();
  }
}

void NotificationCard::changeEvent(QEvent *event)
{
  QFrame::changeEvent(event);
  if (event->type() == QEvent::PaletteChange ||
      event->type() == QEvent::ApplicationPaletteChange) {
    updateShadowEffect();
    update();
  }
}

void NotificationCard::onTick()
{
  if (m_isHovered)
    return;

  m_remainingMs -= kTickIntervalMs;
  if (m_remainingMs <= 0) {
    m_remainingMs = 0;
    if (m_tickTimer) {
      m_tickTimer->stop();
    }
    dismiss();
  } else {
    update(0, height() - 4, width(), 4);
  }
}

void NotificationCard::onCloseClicked()
{
  dismiss();
}

void NotificationCard::onCopyClicked()
{
  QString textToCopy = m_message;
  if (!m_detail.isEmpty()) {
    textToCopy += QStringLiteral("\n\n---\n") + m_detail;
  }
  QGuiApplication::clipboard()->setText(textToCopy);
  if (m_copyBtn) {
    m_copyBtn->setText(tr("已复制"));
    QTimer::singleShot(1500, this, [this] {
      if (m_copyBtn)
        m_copyBtn->setText(tr("复制"));
    });
  }
}

void NotificationCard::updateAggregation(int count, const QString &latestMessage)
{
  m_aggregationCount = count;
  if (!latestMessage.isEmpty()) {
    m_message = latestMessage;
    if (m_messageLabel)
      m_messageLabel->setText(m_message);
  }
  if (m_badgeLabel) {
    m_badgeLabel->setText(QStringLiteral("×%1").arg(m_aggregationCount));
    m_badgeLabel->setVisible(m_aggregationCount > 1);
  }
  restartTimeout();
}

void NotificationCard::resetTimeout(int newTimeoutMs)
{
  if (newTimeoutMs > 0)
    m_totalTimeoutMs = newTimeoutMs;
  m_remainingMs = m_totalTimeoutMs;
  if (!m_isHovered && m_tickTimer && m_totalTimeoutMs > 0) {
    m_tickTimer->start();
  }
  update();
}

void NotificationCard::restartTimeout()
{
  resetTimeout(m_totalTimeoutMs);
}

void NotificationCard::dismiss()
{
  if (m_tickTimer) {
    m_tickTimer->stop();
  }
  emit dismissed(this);
}

} // namespace paleo::ui
