// 层：视图
#include "paleostartupscreen.h"
#include "paleotheme.h"

#include <QGuiApplication>
#include <QLabel>
#include <QProgressBar>
#include <QScreen>
#include <QVBoxLayout>

PaleoStartupScreen::PaleoStartupScreen()
    : QWidget(nullptr, Qt::SplashScreen | Qt::FramelessWindowHint)
{
  setObjectName(QStringLiteral("paleoStartupScreen"));
  setWindowTitle(tr("正在启动 Paleo Workbench"));
  setAccessibleName(windowTitle());
  setAttribute(Qt::WA_QuitOnClose, false);
  const auto &t = PaleoTheme::tokens();
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(t.spacingXl, t.spacingXl, t.spacingXl, t.spacingXl);
  layout->setSpacing(t.spacingSm);
  auto *title = new QLabel(tr("Paleo Workbench"), this);
  title->setObjectName(QStringLiteral("startupTitle"));
  QFont titleFont = PaleoTheme::bodyFont(t.displayPt);
  titleFont.setWeight(QFont::Medium);
  title->setFont(titleFont);
  auto *subtitle = new QLabel(tr("古地理编图工作台"), this);
  subtitle->setObjectName(QStringLiteral("startupSubtitle"));
  layout->addWidget(title);
  layout->addWidget(subtitle);
  layout->addStretch(1);
  m_stage = new QLabel(tr("正在启动…"), this);
  m_stage->setObjectName(QStringLiteral("startupStage"));
  m_stage->setTextFormat(Qt::PlainText);
  m_stage->setWordWrap(true);
  layout->addWidget(m_stage);
  auto *progress = new QProgressBar(this);
  progress->setObjectName(QStringLiteral("startupProgress"));
  progress->setAccessibleName(tr("启动加载进度"));
  progress->setRange(0, 0); // No fabricated percentage for synchronous initialization.
  progress->setTextVisible(false);
  progress->setFixedHeight(t.spacingXs);
  layout->addWidget(progress);
  PaleoTheme::applyThemedStyleSheet(this, [] {
    const auto &colors = PaleoTheme::tokens();
    return QStringLiteral(
        "QWidget#paleoStartupScreen { background: %1; border: 1px solid %2; }"
        "QLabel { color: %3; border: none; }"
        "QLabel#startupSubtitle, QLabel#startupStage { color: %4; }"
        "QProgressBar { background: %5; border: none; }"
        "QProgressBar::chunk { background: %4; }")
        .arg(colors.surface.name(), colors.border.name(), colors.text.name(),
             colors.textMuted.name(), colors.surfaceAltRaised.name());
  });
  resize(28 * t.spacingMd, 16 * t.spacingMd);
  if (QScreen *screen = QGuiApplication::primaryScreen())
  {
    const QRect available = screen->availableGeometry();
    resize(size().boundedTo(available.size()));
    move(available.center() - rect().center());
  }
}

void PaleoStartupScreen::setStage(const QString &text)
{
  m_stage->setText(text);
  m_stage->setAccessibleName(text);
  if (isVisible())
    repaint();
}
