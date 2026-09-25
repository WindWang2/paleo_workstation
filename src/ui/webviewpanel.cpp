#include "webviewpanel.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QStackedLayout>
#include <QVBoxLayout>

#include <QWebEnginePage>
#include <QWebEngineView>

// DESIGN.md tokens: status/fallback text uses text-muted #5D6E80.
static const char kMutedStyle[] = "color: #5D6E80;";

WebViewPanel::WebViewPanel(QWidget *parent)
  : QWidget(parent)
{
  auto *stack = new QStackedLayout(this);
  stack->setContentsMargins(0, 0, 0, 0);

  // Page 0 — status/fallback surface (placeholder or error + external open).
  auto *statusPage = new QWidget(this);
  auto *lay = new QVBoxLayout(statusPage);
  lay->setContentsMargins(12, 12, 12, 12);
  lay->addStretch(1);
  m_statusLabel = new QLabel(tr("还没有打开的 web 服务"), statusPage);
  m_statusLabel->setObjectName(QStringLiteral("webStatus"));
  m_statusLabel->setAlignment(Qt::AlignCenter);
  m_statusLabel->setWordWrap(true);
  m_statusLabel->setStyleSheet(QLatin1String(kMutedStyle));
  lay->addWidget(m_statusLabel);
  m_externalButton = new QPushButton(tr("用系统浏览器打开"), statusPage);
  m_externalButton->setObjectName(QStringLiteral("webExternalButton"));
  m_externalButton->setVisible(false);
  connect(m_externalButton, &QPushButton::clicked, this, [this] {
    if (m_url.isValid())
      QDesktopServices::openUrl(m_url);
  });
  lay->addWidget(m_externalButton, 0, Qt::AlignHCenter);
  lay->addStretch(1);
  stack->addWidget(statusPage);
}

bool WebViewPanel::setUrl(const QUrl &url)
{
  m_url = url;
  if (!url.isValid() || url.isEmpty())
  {
    m_lastError = tr("无效的 URL");
    showFallback(m_lastError); // visible feedback, not a silent no-op
    emit loadFailed(m_lastError);
    return false;
  }

  QString err;
  if (!ensureEngine(&err))
  {
    m_lastError = err;
    showFallback(err);
    emit loadFailed(err);
    return false;
  }

  m_engine->setUrl(url);
  if (auto *stack = qobject_cast<QStackedLayout *>(layout()))
    stack->setCurrentWidget(m_engine);
  return true;
}

bool WebViewPanel::ensureEngine(QString *error)
{
  if (m_engine)
    return true;

  // Headless/offscreen (ctest, CI) cannot host a Chromium compositor — fail
  // deterministically before touching QtWebEngine classes.
  const QString platform = QGuiApplication::platformName();
  if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal"))
  {
    if (error)
      *error = tr("无屏平台（%1）不支持内嵌浏览器").arg(platform);
    return false;
  }

  m_engine = new QWebEngineView(this);
  auto *stack = qobject_cast<QStackedLayout *>(layout());
  if (stack)
    stack->addWidget(m_engine);

  connect(m_engine, &QWebEngineView::loadFinished, this,
          &WebViewPanel::loadFinished);
  // Render-process death is recoverable for the host: swap to the fallback
  // surface and report — the panel must never take the app down with it.
  connect(m_engine->page(), &QWebEnginePage::renderProcessTerminated, this,
          [this](QWebEnginePage::RenderProcessTerminationStatus status, int code) {
            const QString reason =
                tr("渲染进程异常终止（status=%1, code=%2）").arg(status).arg(code);
            m_lastError = reason;
            showFallback(reason);
            emit loadFailed(reason);
          });
  return true;
}

void WebViewPanel::showFallback(const QString &reason)
{
  if (m_statusLabel)
    m_statusLabel->setText(tr("内嵌浏览器不可用：%1").arg(reason));
  if (m_externalButton)
    m_externalButton->setVisible(m_url.isValid() && !m_url.isEmpty());
  if (auto *stack = qobject_cast<QStackedLayout *>(layout()))
    stack->setCurrentIndex(0);
}
