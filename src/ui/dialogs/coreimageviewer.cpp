// 层：视图
#include "coreimageviewer.h"
#include "../../domain/wellcompositemodel.h"
#include "../../services/imagelod.h"
#include "../paleotheme.h"
#include "../paleoviewport.h"
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QUuid>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <functional>

namespace {
QString tr(const char *text) { return QCoreApplication::translate("CoreImageViewer", text); }

class PhotoView final : public QGraphicsView {
public:
  explicit PhotoView(QWidget *parent) : QGraphicsView(parent) {
    setObjectName(QStringLiteral("coreImageView"));
    setAccessibleName(::tr("岩心照片放大视图"));
    setScene(new QGraphicsScene(this));
    setRenderHint(QPainter::SmoothPixmapTransform);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
  }
  std::function<void(double)> zoomChanged;
  void setPhoto(QPixmap pixmap) {
    const QSizeF oldSize = sceneRect().size();
    const QPointF oldCenter = mapToScene(viewport()->rect().center());
    pixmap.setDevicePixelRatio(1.0);
    scene()->clear();
    scene()->addPixmap(pixmap);
    setSceneRect(QRectF(QPointF(), pixmap.size()));
    setBackgroundBrush(pixmap.hasAlpha() ? paleo::imagelod::alphaCheckerboard() : QBrush());
    if (m_fit || oldSize.isEmpty()) fitPhoto();
    else {
      const double ratio = oldSize.width() / pixmap.width();
      scale(ratio, ratio);
      centerOn(oldCenter.x() / oldSize.width() * pixmap.width(),
               oldCenter.y() / oldSize.height() * pixmap.height());
      notifyZoom();
    }
  }
  void fitPhoto() {
    m_fit = true;
    if (sceneRect().isEmpty()) return;
    fitInView(sceneRect(), Qt::KeepAspectRatio);
    notifyZoom();
  }
  void originalSize() {
    m_fit = false;
    resetTransform();
    notifyZoom();
  }
  void zoomBy(double factor) {
    if (sceneRect().isEmpty()) return;
    m_fit = false;
    const double current = transform().m11();
    const double next = qBound(0.01, current * factor, 16.0);
    scale(next / current, next / current);
    notifyZoom();
  }
protected:
  void wheelEvent(QWheelEvent *event) override {
    if (!event->angleDelta().y()) { QGraphicsView::wheelEvent(event); return; }
    zoomBy(std::pow(1.2, event->angleDelta().y() / 120.0));
    event->accept();
  }
  void resizeEvent(QResizeEvent *event) override {
    QGraphicsView::resizeEvent(event);
    if (m_fit) fitPhoto();
  }
private:
  void notifyZoom() { if (zoomChanged) zoomChanged(transform().m11()); }
  bool m_fit = true;
};

class PhotoDialog final : public QDialog {
public:
  PhotoDialog(const WellComposite::ImageDepthItem &item, const QString &wellName, QWidget *parent)
      : QDialog(parent) {
    setObjectName(QStringLiteral("coreImageViewer"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::WindowMaximizeButtonHint);
    const QString name = item.caption.isEmpty() ? QFileInfo(item.imagePath).fileName() : item.caption;
    setWindowTitle(::tr("岩心照片 — %1").arg(wellName.isEmpty() ? name : wellName + " · " + name));
    resize(900, 700);
    auto *layout = new QVBoxLayout(this);
    const int margin = PaleoTheme::tokens().spacingMd;
    layout->setContentsMargins(margin, margin, margin, margin);
    layout->setSpacing(PaleoTheme::tokens().spacingSm);
    auto *depth = new QLabel(item.bottomDepth > item.topDepth
        ? ::tr("测深：%1–%2 m").arg(item.topDepth, 0, 'f', 2).arg(item.bottomDepth, 0, 'f', 2)
        : ::tr("锚深：%1 m").arg(item.topDepth, 0, 'f', 2), this);
    depth->setFont(PaleoTheme::monoFont());
    layout->addWidget(depth);
    m_view = new PhotoView(this);
    layout->addWidget(makeControls(), 0);
    layout->addWidget(m_view, 1);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("coreImageStatus"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status);
    auto *hint = new QLabel(::tr("滚轮缩放，按住左键拖动浏览；关闭窗口返回井道。"), this);
    PaleoTheme::applyThemedStyleSheet(hint, [] { return PaleoTheme::mutedCaptionStyleSheet(); });
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
    if (!item.pixmap.isNull()) m_view->setPhoto(item.pixmap);
    loadOriginal(item);
  }
private:
  QWidget *makeControls() {
    auto *bar = new QWidget(this);
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(PaleoTheme::tokens().spacingSm);
    const auto button = [this, row, bar](const QString &text, auto action) {
      auto *b = new QPushButton(text, bar);
      b->setAccessibleName(text);
      connect(b, &QPushButton::clicked, m_view, action);
      row->addWidget(b);
      return b;
    };
    button(::tr("放大"), [this] { m_view->zoomBy(1.2); });
    button(::tr("缩小"), [this] { m_view->zoomBy(1.0 / 1.2); });
    button(::tr("适应窗口"), [this] { m_view->fitPhoto(); });
    m_original = button(::tr("原始大小（100%）"), [this] { m_view->originalSize(); });
    m_original->setEnabled(false);
    m_original->setToolTip(::tr("原图读取完成后可查看原始大小"));
    auto *zoom = new QLabel(bar);
    zoom->setFont(PaleoTheme::monoFont());
    row->addWidget(zoom);
    row->addStretch();
    m_view->zoomChanged = [zoom](double factor) { zoom->setText(QStringLiteral("%1%").arg(qRound(factor * 100))); };
    return new PaleoToolRow(bar, this);
  }
  void loadOriginal(const WellComposite::ImageDepthItem &item) {
    if (item.imagePath.isEmpty()) {
      m_status->setText(item.pixmap.isNull() ? ::tr("没有可显示的照片。")
                                          : ::tr("仅有内嵌图片，按现有分辨率显示。"));
      m_original->setEnabled(!item.pixmap.isNull());
      m_original->setToolTip(item.pixmap.isNull() ? ::tr("没有可显示的照片。") : QString());
      return;
    }
    m_status->setText(::tr("正在读取原图…"));
    const QPointer<PhotoDialog> guard(this);
    // 每次检视有自己的请求，不能因另一窗口正在解码同一文件而丢掉完成通知。
    const QString key = QStringLiteral("core-viewer|%1").arg(QUuid::createUuid().toString());
    const bool queued = paleo::imagelod::FullImageCache::shared().requestDecode(
        key, item.imagePath, [guard](const QImage &image) {
          if (!guard) return;
          if (image.isNull()) {
            guard->m_status->setText(::tr("无法读取原图；如有缩略图，可继续查看缩略图。"));
            return;
          }
          guard->m_view->setPhoto(QPixmap::fromImage(image));
          guard->m_original->setEnabled(true);
          guard->m_original->setToolTip(QString());
          guard->m_status->setText(::tr("原图：%1 × %2 像素").arg(image.width()).arg(image.height()));
        });
    if (!queued) m_status->setText(::tr("无法启动原图读取；请关闭窗口后重试。"));
  }
  PhotoView *m_view = nullptr;
  QLabel *m_status = nullptr;
  QPushButton *m_original = nullptr;
};
} // namespace

void PaleoCoreImageViewer::open(const WellComposite::ImageDepthItem &item,
                                const QString &wellName, QWidget *parent) {
  auto *dialog = new PhotoDialog(item, wellName, parent);
  dialog->show();
}
