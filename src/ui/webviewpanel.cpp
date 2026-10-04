// 层：视图
#include "webviewpanel.h"
#include "paleotheme.h"

#include <QDesktopServices>
#include <QGuiApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedLayout>
#include <QVBoxLayout>

#if PALEO_HAVE_WEBENGINE
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineDownloadRequest>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineView>
#endif

// DESIGN.md tokens: status/fallback text uses text-muted #5D6E80.
// 状态文字次级色（DESIGN.md text-muted）——从 PaleoTheme 现取（随主题翻转），
// 活体注册：切主题自动重算。
static void applyMutedStyle(QWidget *w)
{
  PaleoTheme::applyThemedStyleSheet(w, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
}

WebViewPanel::WebViewPanel(QWidget *parent)
  : QWidget(parent)
{
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  m_stack = new QStackedLayout;
  m_stack->setContentsMargins(0, 0, 0, 0);
  root->addLayout(m_stack, 1);

  // 加载进度条：大页面加载超过 1s 时的唯一反馈。极薄一条，常驻布局、
  // 闲时隐藏，避免显隐引起内容区跳动。
  m_progress = new QProgressBar(this);
  m_progress->setObjectName(QStringLiteral("webLoadProgress"));
  m_progress->setRange(0, 100);
  m_progress->setValue(0);
  m_progress->setTextVisible(false);
  m_progress->setFixedHeight(3);
  m_progress->setVisible(false);
  root->addWidget(m_progress);

  // Page 0 — status/fallback surface (placeholder or error + external open).
  auto *statusPage = new QWidget(this);
  auto *lay = new QVBoxLayout(statusPage);
  lay->setContentsMargins(PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd, PaleoTheme::tokens().spacingMd);
  lay->addStretch(1);
  m_statusLabel = new QLabel(tr("还没有打开的 web 服务"), statusPage);
  m_statusLabel->setObjectName(QStringLiteral("webStatus"));
  m_statusLabel->setAlignment(Qt::AlignCenter);
  m_statusLabel->setWordWrap(true);
  applyMutedStyle(m_statusLabel);
  lay->addWidget(m_statusLabel);
  m_externalButton = new QPushButton(tr("用系统浏览器打开"), statusPage);
  m_externalButton->setObjectName(QStringLiteral("webExternalButton"));
  m_externalButton->setVisible(false);
  connect(m_externalButton, &QPushButton::clicked, this, [this] {
    if (isAllowedUrl(m_url)) // 外部打开前用同一份白名单再校验一次（#82）
      QDesktopServices::openUrl(m_url);
  });
  lay->addWidget(m_externalButton, 0, Qt::AlignHCenter);
  lay->addStretch(1);
  m_stack->addWidget(statusPage);
}

bool WebViewPanel::isAllowedUrl(const QUrl &url)
{
  if (!url.isValid() || url.isEmpty() || url.host().isEmpty())
    return false;
  const QString scheme = url.scheme().toLower();
  return scheme == QLatin1String("http") || scheme == QLatin1String("https");
}

WebViewPanel::~WebViewPanel()
{
#if PALEO_HAVE_WEBENGINE
  // QWebEngineProfile 必须比使用它的 page 活得更久。
  delete m_engine;
  delete m_profile;
#endif
}

void WebViewPanel::setPageStyleSheet(const QString &css)
{
  m_pageCss = css;
  installPageStyleSheet();
}

void WebViewPanel::installPageStyleSheet()
{
#if PALEO_HAVE_WEBENGINE
  if (!m_engine)
    return;
  const QString name = QStringLiteral("paleoHostTheme");
  auto &scripts = m_engine->page()->scripts();
  for (const auto &previous : scripts.find(name))
    scripts.remove(previous);
  QWebEngineScript script;
  script.setName(name);
  script.setInjectionPoint(QWebEngineScript::DocumentReady);
  script.setWorldId(QWebEngineScript::ApplicationWorld);
  script.setRunsOnSubFrames(false);
  const QString css = QString::fromUtf8(QJsonDocument(QJsonArray{m_pageCss}).toJson(QJsonDocument::Compact));
  script.setSourceCode(QStringLiteral(
      "(() => { let s=document.getElementById('paleo-host-theme'); "
      "if(!s){s=document.createElement('style');s.id='paleo-host-theme';"
      "(document.head||document.documentElement).appendChild(s);}s.textContent=%1[0]; })();").arg(css));
  scripts.insert(script);
  m_engine->page()->runJavaScript(script.sourceCode(), QWebEngineScript::ApplicationWorld);
#endif
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
  if (!isAllowedUrl(url))
  {
    // #82：本面板只嵌「已运行的 web 服务」——非 http/https 不加载，降级面
    // 也不提供外部打开（m_url 清空 → showFallback 隐藏外部按钮）。
    m_url = QUrl();
    m_lastError = tr("仅支持 http:// 或 https:// 服务地址（已拒绝 %1:）").arg(url.scheme());
    showFallback(m_lastError);
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

#if PALEO_HAVE_WEBENGINE
  m_lastError.clear();
  m_engine->setUrl(url);
  if (m_stack)
    m_stack->setCurrentWidget(m_engine);
#endif
  return true;
}

bool WebViewPanel::ensureEngine(QString *error)
{
  if (m_engine)
    return true;

#if !PALEO_HAVE_WEBENGINE
  // D14：构建期就没有 Qt WebEngine —— 走既有降级面（外部浏览器兜底），
  // 不碰任何 QtWebEngine 类。首查先于平台检查：错因如实说构建形态。
  if (error)
    *error = tr("本构建未启用内嵌浏览器（缺少 Qt WebEngine 组件）");
  return false;
#else
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
  m_engine->setObjectName(QStringLiteral("embeddedWebEngine"));
  // 命名 profile 将 IndexedDB 恢复草稿留在 Qt 用户数据目录；各宿主页
  // 隔离页面存储/下载信号，切页保留同一 profile。
  m_profile = new QWebEngineProfile(QStringLiteral("paleoWebView.") + objectName(), this);
  m_engine->setPage(new QWebEnginePage(m_profile, m_engine));
  installPageStyleSheet();
  connect(m_profile, &QWebEngineProfile::downloadRequested, this,
          [this](QWebEngineDownloadRequest *download) {
    const QString path = QFileDialog::getSaveFileName(this, tr("保存 Web 工作台成果"),
        QDir(download->downloadDirectory()).filePath(download->downloadFileName()));
    if (path.isEmpty())
    {
      download->cancel();
      return;
    }
    download->setDownloadDirectory(QFileInfo(path).absolutePath());
    download->setDownloadFileName(QFileInfo(path).fileName());
    download->accept();
  });
  if (m_stack)
    m_stack->addWidget(m_engine);

  // 加载反馈：开始时亮出进度条，结束（成败都算）收起。
  connect(m_engine, &QWebEngineView::loadStarted, this, [this] {
    if (m_progress)
    {
      m_progress->setValue(0);
      m_progress->setVisible(true);
    }
  });
  connect(m_engine, &QWebEngineView::loadProgress, this, [this](int p) {
    if (m_progress)
      m_progress->setValue(p);
  });
  connect(m_engine, &QWebEngineView::loadFinished, this, [this](bool ok) {
    if (m_progress)
      m_progress->setVisible(false);
    if (ok)
    {
      m_lastError.clear();
      m_stack->setCurrentWidget(m_engine);
    }
    else
    {
      m_lastError = tr("页面加载失败；请检查服务是否已启动，然后重新连接");
      showFallback(m_lastError);
      emit loadFailed(m_lastError);
    }
  });
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
#endif // PALEO_HAVE_WEBENGINE
}

void WebViewPanel::showFallback(const QString &reason)
{
  if (m_progress)
    m_progress->setVisible(false);
  if (m_statusLabel)
    m_statusLabel->setText(tr("内嵌页面不可用：%1").arg(reason));
  if (m_externalButton)
    m_externalButton->setVisible(isAllowedUrl(m_url));
  if (m_stack)
    m_stack->setCurrentIndex(0);
}

void WebViewPanel::showError(const QUrl &url, const QString &reason)
{
  m_url = isAllowedUrl(url) ? url : QUrl();
  m_lastError = reason;
  showFallback(reason);
}
