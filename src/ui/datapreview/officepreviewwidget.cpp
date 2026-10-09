// 层：视图
#include "officepreviewwidget.h"
#include "../../workflow/officepreviewsession.h"
#include "../paleotheme.h"
#include "../webviewpanel.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

OfficePreviewWidget::OfficePreviewWidget(const QString &path, const QString &expectedSha, QWidget *parent)
    : QWidget(parent), m_path(path), m_sha(expectedSha), m_session(new OfficePreviewSession(this)),
      m_web(new WebViewPanel(this)), m_status(new QLabel(this)), m_retry(new QPushButton(tr("重试"), this))
{
  setObjectName(QStringLiteral("officePreview"));
  setAccessibleName(tr("Office 原件"));
  auto *layout = new QVBoxLayout(this);
  const auto &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(tokens.spacingSm);
  m_status->setObjectName(QStringLiteral("officeStatus"));
  m_status->setWordWrap(true);
  m_status->setAlignment(Qt::AlignCenter);
  m_status->setAccessibleName(tr("编辑状态"));
  PaleoTheme::applyThemedStyleSheet(m_status, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
  m_retry->setObjectName(QStringLiteral("officeRetry"));
  m_retry->hide();
  auto *bar = new QHBoxLayout;
  bar->setContentsMargins(tokens.spacingSm, tokens.spacingSm, tokens.spacingSm, 0);
  bar->addWidget(m_status, 1);
  bar->addWidget(m_retry);
  layout->addLayout(bar);
  m_web->setObjectName(QStringLiteral("officeEditor"));
  m_web->setOffTheRecord(true);
  layout->addWidget(m_web, 1);
  connect(m_retry, &QPushButton::clicked, this, &OfficePreviewWidget::open);
  connect(m_session, &OfficePreviewSession::ready, this, [this](const QUrl &url) {
    showMessage(tr("正在打开文档…"));
    m_retry->hide();
    m_web->setUrl(url);
  });
  connect(m_session, &OfficePreviewSession::failed, this, [this](const QString &reason) {
    showMessage(reason);
    m_retry->show();
  });
  connect(m_session, &OfficePreviewSession::documentSaved, this, [this](const QString &path) {
    showMessage(tr("已收到编辑内容"));
    emit editSaved(path);
  });
  connect(m_web, &WebViewPanel::loadFinished, this, [this](bool ok) {
    if (ok)
      showMessage(QString());
    else
    {
      showMessage(tr("文档页面没有打开"));
      m_retry->show();
    }
  });
  connect(m_web, &WebViewPanel::loadFailed, this, [this](const QString &reason) {
    showMessage(reason);
    m_retry->show();
  });
  open();
}

OfficePreviewWidget::~OfficePreviewWidget()
{
  m_session->stop();
  emit previewClosed();
}

void OfficePreviewWidget::showMessage(const QString &text)
{
  m_status->setText(text);
  m_status->setVisible(!text.isEmpty());
}

void OfficePreviewWidget::open()
{
  showMessage(tr("正在准备文档…"));
  m_retry->hide();
  m_session->open(m_path, m_sha);
}
