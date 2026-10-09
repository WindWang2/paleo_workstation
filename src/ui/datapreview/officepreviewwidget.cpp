// 层：视图
#include "officepreviewwidget.h"
#include "../../workflow/officepreviewsession.h"
#include "../paleotheme.h"
#include <QComboBox>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QString officeStatusStyleSheet(bool error)
{
  if (!error) return PaleoTheme::mutedCaptionStyleSheet();
  return QStringLiteral("color: %1;").arg(PaleoTheme::tokens().errorText.name().toUpper());
}
}

OfficePreviewWidget::OfficePreviewWidget(const QString &path, const QString &expectedSha, QWidget *parent)
    : QWidget(parent), m_path(path), m_sha(expectedSha), m_session(new OfficePreviewSession(this)),
      m_status(new QLabel(this)), m_retry(new QPushButton(tr("重试"), this)),
      m_previous(new QToolButton(this)), m_next(new QToolButton(this)),
      m_pages(new QComboBox(this)), m_zoom(new QComboBox(this)),
      m_view(new QGraphicsView(this))
{
  setObjectName(QStringLiteral("officePreview"));
  setAccessibleName(tr("Office 原件预览"));
  auto *layout = new QVBoxLayout(this);
  const auto &tokens = PaleoTheme::tokens();
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(tokens.spacingSm);
  auto *toolbar = new QHBoxLayout;
  toolbar->setContentsMargins(tokens.spacingSm, tokens.spacingSm, tokens.spacingSm, 0);
  toolbar->setSpacing(tokens.spacingSm);
  m_previous->setText(tr("上一页")); m_next->setText(tr("下一页"));
  PaleoTheme::applyThemedStyleSheet(m_previous, [] { return PaleoTheme::toolButtonStyleSheet(); });
  PaleoTheme::applyThemedStyleSheet(m_next, [] { return PaleoTheme::toolButtonStyleSheet(); });
  m_pages->setObjectName(QStringLiteral("officePageSelector"));
  m_pages->setAccessibleName(tr("文档页码或工作表"));
  m_pages->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  m_pages->setMinimumContentsLength(12);
  m_zoom->setObjectName(QStringLiteral("officeZoom"));
  m_zoom->setAccessibleName(tr("缩放"));
  m_zoom->addItem(tr("适合宽度"), 0);
  for (int percent : {50, 75, 100, 125, 150, 200}) m_zoom->addItem(tr("%1%").arg(percent), percent);
  toolbar->addWidget(m_previous); toolbar->addWidget(m_pages, 1); toolbar->addWidget(m_next);
  toolbar->addWidget(m_zoom);
  layout->addLayout(toolbar);
  m_status->setWordWrap(true); m_status->setAlignment(Qt::AlignCenter);
  m_status->setAccessibleName(tr("预览状态"));
  PaleoTheme::applyThemedStyleSheet(m_status, [this] { return officeStatusStyleSheet(m_statusIsError); });
  m_statusHost = new QWidget(this);
  auto *statusLayout = new QVBoxLayout(m_statusHost);
  statusLayout->setContentsMargins(tokens.spacingSm, 0, tokens.spacingSm, 0);
  statusLayout->setSpacing(tokens.spacingXs);
  statusLayout->addStretch(1);
  statusLayout->addWidget(m_status);
  statusLayout->addWidget(m_retry, 0, Qt::AlignHCenter);
  statusLayout->addStretch(1);
  layout->addWidget(m_statusHost, 1);
  auto *scene = new QGraphicsScene(m_view);
  m_image = scene->addPixmap(QPixmap());
  m_view->setScene(scene);
  m_view->setObjectName(QStringLiteral("officePageView"));
  m_view->setAccessibleName(tr("文档页面"));
  m_view->setBackgroundRole(QPalette::AlternateBase);
  m_view->setRenderHint(QPainter::SmoothPixmapTransform);
  m_view->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
  m_view->setDragMode(QGraphicsView::ScrollHandDrag);
  m_view->viewport()->installEventFilter(this);
  layout->addWidget(m_view, 1);
  connect(this, &OfficePreviewWidget::previewRequested, m_session, &OfficePreviewSession::open);
  connect(this, &OfficePreviewWidget::pageRequested, m_session, &OfficePreviewSession::requestPage);
  connect(this, &OfficePreviewWidget::previewClosed, m_session, &OfficePreviewSession::stop);
  connect(m_retry, &QPushButton::clicked, this, &OfficePreviewWidget::open);
  connect(m_previous, &QToolButton::clicked, this, [this] { m_pages->setCurrentIndex(m_pages->currentIndex() - 1); });
  connect(m_next, &QToolButton::clicked, this, [this] { m_pages->setCurrentIndex(m_pages->currentIndex() + 1); });
  connect(m_pages, &QComboBox::currentIndexChanged, this, &OfficePreviewWidget::selectPage);
  connect(m_zoom, &QComboBox::currentIndexChanged, this, &OfficePreviewWidget::updateZoom);
  connect(m_session, &OfficePreviewSession::failed, this, [this](const QString &error) {
    m_image->setPixmap(QPixmap()); m_view->scene()->setSceneRect(QRectF());
    showStatus(error, true);
    m_pages->setEnabled(false); m_previous->setEnabled(false); m_next->setEnabled(false);
  });
  connect(m_session, &OfficePreviewSession::ready, this, [this](const QStringList &labels) {
    { const QSignalBlocker blocker(m_pages); m_pages->clear(); m_pages->addItems(labels); }
    m_pages->setEnabled(true);
    selectPage(0);
  });
  connect(m_session, &OfficePreviewSession::pageReady, this, [this](int index, const QImage &image) {
    if (index != m_pages->currentIndex()) return;
    m_pageScale = image.dotsPerMeterX() > 0 ? logicalDpiX() / (image.dotsPerMeterX() * 0.0254) : 1.0;
    m_image->setPixmap(QPixmap::fromImage(image));
    m_view->scene()->setSceneRect(m_image->boundingRect());
    m_statusHost->hide(); m_retry->hide(); m_view->show();
    updateZoom();
  });
  showStatus(tr("正在打开 Office 原件…"), false);
  QTimer::singleShot(0, this, &OfficePreviewWidget::open);
}
OfficePreviewWidget::~OfficePreviewWidget() { emit previewClosed(); }
void OfficePreviewWidget::open()
{
  { const QSignalBlocker blocker(m_pages); m_pages->clear(); }
  m_pages->setEnabled(false); m_previous->setEnabled(false); m_next->setEnabled(false);
  m_image->setPixmap(QPixmap()); m_view->scene()->setSceneRect(QRectF());
  showStatus(tr("正在打开 Office 原件…"), false);
  emit previewRequested(m_path, m_sha);
}
void OfficePreviewWidget::showStatus(const QString &text, bool error)
{
  m_statusIsError = error;
  m_status->setText(text);
  m_status->setStyleSheet(officeStatusStyleSheet(error));
  m_status->show();
  m_retry->setVisible(error);
  m_statusHost->show();
  m_view->setVisible(!m_image->pixmap().isNull());
}
void OfficePreviewWidget::selectPage(int index)
{
  if (index < 0) return;
  m_previous->setEnabled(index > 0); m_next->setEnabled(index + 1 < m_pages->count());
  m_image->setPixmap(QPixmap());
  m_view->scene()->setSceneRect(QRectF());
  showStatus(tr("正在加载第 %1 / %2 页…").arg(index + 1).arg(m_pages->count()), false);
  emit pageRequested(index);
}
void OfficePreviewWidget::resizeEvent(QResizeEvent *event)
{
  QWidget::resizeEvent(event);
  if (m_zoom->currentData().toInt() == 0) updateZoom();
}
void OfficePreviewWidget::updateZoom()
{
  if (m_image->pixmap().isNull()) return;
  const int percent = m_zoom->currentData().toInt();
  const double scale = percent ? percent / 100.0 * m_pageScale
    : qMax(1, m_view->viewport()->width() - 2 * PaleoTheme::tokens().spacingSm) / m_image->boundingRect().width();
  m_view->setTransform(QTransform::fromScale(scale, scale));
}
bool OfficePreviewWidget::eventFilter(QObject *object, QEvent *event)
{
  if (object == m_view->viewport() && event->type() == QEvent::Resize && m_zoom->currentData().toInt() == 0)
    QTimer::singleShot(0, this, &OfficePreviewWidget::updateZoom);
  return QWidget::eventFilter(object, event);
}
