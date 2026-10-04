// 层：视图
#include "stratigraphicwebpage.h"

#include "../paleoicons.h"
#include "../paleotheme.h"
#include "../webviewpanel.h"
#include "../../workflow/stratigraphicwebsession.h"

#include <SARibbon.h>
#include <QAction>
#include <QDesktopServices>
#include <QFileDialog>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

StratigraphicWebPage::StratigraphicWebPage(StratigraphicWebSession *session, QWidget *parent)
    : QWidget(parent), m_session(session)
{
  setObjectName(QStringLiteral("stratigraphicWebPage"));
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  m_status = new QLabel(tr("连接已有工作台，或在 ribbon 选择独立项目后启动服务。"), this);
  m_status->setObjectName(QStringLiteral("correlationWebStatus"));
  m_status->setTextFormat(Qt::PlainText);
  m_status->setWordWrap(true);
  m_status->setMargin(PaleoTheme::tokens().spacingSm);
  m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
  PaleoTheme::applyThemedStyleSheet(m_status, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  layout->addWidget(m_status);
  m_web = new WebViewPanel(this);
  m_web->setObjectName(QStringLiteral("correlationWebView"));
  // 在宿主中适配 Web chrome；独立源码和 SVG 地质数据符号无需修改。
  const auto applyWebTheme = [this] {
    const auto &t = PaleoTheme::tokens();
    m_web->setPageStyleSheet(QStringLiteral(
        ":root { --ink:%1; --muted:%2; --blue:%3; --line:%4; --bg:%5; }"
        "body { color:%1; background:%5; font-family:'Noto Sans SC','Noto Sans CJK SC',"
        "'Microsoft YaHei UI',sans-serif; font-size:%6pt; }"
        "button,input,select,textarea { font:inherit; color:%1; border-color:%4; "
        "background:%7; border-radius:%8px; }"
        "button:hover { background:%5; border-color:%4; }"
        "button.primary { background:%3; color:%9; border-color:%3; }"
        ".workflow-steps button.active { color:%10; border-bottom-color:%3; background:%7; }"
        "button.active,button[aria-pressed=true] { color:%10; border-color:%3; }"
        ".brand-mark { color:%2; }"
        "button:focus-visible,input:focus-visible,select:focus-visible,summary:focus-visible "
        "{ outline:2px solid %10; outline-offset:2px; }"
        ".app-header,.workspace-toolbar,.status-bar,.menu-panel,dialog,.sidebar,.inspector,"
        ".bottom-panel,.stage-heading,.workflow-steps { background:%7; color:%1; border-color:%4; }"
        ".muted { color:%2; }")
        .arg(t.text.name(), t.textMuted.name(), t.primary.name(), t.border.name(),
             t.surfaceAlt.name()).arg(t.bodyPt).arg(t.surface.name()).arg(t.radiusSm)
        .arg(t.onPrimary.name(), t.focusRing.name()));
  };
  // 同主系统使用活体主题注册，隐藏页也随主题同步。
  PaleoTheme::applyThemedStyleSheet(m_web, [applyWebTheme] {
    applyWebTheme();
    return QString();
  });
  layout->addWidget(m_web, 1);
  connect(session, &StratigraphicWebSession::statusChanged, m_status, &QLabel::setText);
  connect(session, &StratigraphicWebSession::ready, this, [this](const QUrl &url) {
    m_web->setUrl(url);
  });
  connect(session, &StratigraphicWebSession::failed, this, [this](const QString &reason) {
    m_status->setText(reason);
    m_web->showError(m_session->endpoint(), reason);
  });
  connect(m_web, &WebViewPanel::loadFailed, m_status, &QLabel::setText);
}

void StratigraphicWebPage::activate()
{
  // 保留解释页、缩放、滚动、撤销历史；切回页面不重新加载。
  if (!m_activated)
  {
    m_activated = true;
    m_session->connectToService();
  }
}

void StratigraphicWebPage::buildRibbon(SARibbonCategory *category)
{
  auto *connection = category->addPanel(tr("工作台连接"));
  auto *addressHost = new QWidget(connection);
  auto *addressLayout = new QVBoxLayout(addressHost);
  addressLayout->setContentsMargins(PaleoTheme::tokens().spacingSm, 0,
                                   PaleoTheme::tokens().spacingSm, 0);
  addressLayout->setSpacing(PaleoTheme::tokens().spacingXs);
  auto *label = new QLabel(tr("服务地址"), addressHost);
  addressLayout->addWidget(label);
  m_address = new QLineEdit(m_session->endpoint().toString(), addressHost);
  m_address->setObjectName(QStringLiteral("correlationWebAddress"));
  m_address->setAccessibleName(tr("地层对比服务地址"));
  m_address->setMinimumWidth(m_address->fontMetrics().averageCharWidth() * 48);
  addressLayout->addWidget(m_address);
  connection->addLargeWidget(addressHost);
  const auto action = [this](SARibbonPanel *panel, const QString &text, const char *name,
                             const char *icon, const QString &tip) {
    auto *result = new QAction(PaleoIcons::qgisTheme(QLatin1String(icon)), text, this);
    result->setObjectName(QLatin1String(name));
    result->setToolTip(tip);
    panel->addLargeAction(result);
    connect(m_session, &StratigraphicWebSession::busyChanged, result,
            [result, tip](bool busy) {
      result->setEnabled(!busy);
      result->setToolTip(busy ? QObject::tr("正在连接或启动服务，请稍候") : tip);
    });
    return result;
  };
  auto *open = action(connection, tr("连接 / 刷新"), "correlationWebConnectAction",
      "mActionRefresh.svg", tr("连接已运行的地层对比服务；刷新会重新加载页面"));
  const auto connectService = [this] {
    if (m_session->setEndpoint(m_address->text()))
    {
      m_address->setText(m_session->endpoint().toString());
      m_session->connectToService();
    }
  };
  connect(open, &QAction::triggered, this, connectService);
  connect(m_address, &QLineEdit::returnPressed, this, connectService);
  connect(m_session, &StratigraphicWebSession::busyChanged, m_address,
          [this](bool busy) { m_address->setEnabled(!busy); });

  auto *local = category->addPanel(tr("独立服务"));
  auto *directory = action(local, tr("选择独立项目"), "correlationWebDirectoryAction",
      "mActionFileOpen.svg", tr("选择独立交付的 Web 项目目录；路径仅保存在本机配置"));
  connect(directory, &QAction::triggered, this, [this] {
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
      return;
    const QString path = QFileDialog::getExistingDirectory(this, tr("选择地层对比独立项目"),
                                                          m_session->projectDirectory());
    if (!path.isEmpty())
    {
      m_session->setProjectDirectory(path);
      m_status->setText(tr("独立项目：%1。点击「启动服务」打开工作台。").arg(path));
    }
  });
  auto *python = action(local, tr("选择 Python"), "correlationWebPythonAction",
      "mActionOptions.svg", tr("指定已安装独立项目依赖的 Python；默认使用项目 .venv 或系统 Python"));
  connect(python, &QAction::triggered, this, [this] {
    if (QGuiApplication::platformName() == QLatin1String("offscreen"))
      return;
    const QString path = QFileDialog::getOpenFileName(this, tr("选择 Python 可执行文件"),
                                                     m_session->pythonExecutable());
    if (!path.isEmpty())
      m_session->setPythonExecutable(path);
  });
  auto *start = action(local, tr("启动服务"), "correlationWebStartAction",
      "mActionStart.svg", tr("启动选定独立项目；地址已有服务时直接复用"));
  connect(start, &QAction::triggered, this, [this] {
    if (m_session->setEndpoint(m_address->text()))
      m_session->startLocalService();
  });
  auto *browser = category->addPanel(tr("浏览器"));
  auto *external = action(browser, tr("用浏览器打开"), "correlationWebExternalAction",
      "mActionHelpContents.svg", tr("用系统浏览器打开同一地层对比工作台"));
  connect(external, &QAction::triggered, this, [this] {
    if (m_session->setEndpoint(m_address->text()))
      QDesktopServices::openUrl(m_session->endpoint());
  });
}
