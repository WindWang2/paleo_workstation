// 层：视图
#include "datagraphstyle.h"
#include "../paleotheme.h"
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
namespace {
class GraphIcon final : public QIconEngine {
public:
  explicit GraphIcon(QString name) : m_name(std::move(name)) {}
  QIconEngine *clone() const override { return new GraphIcon(m_name); }
  void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
    p->save(); p->setRenderHint(QPainter::Antialiasing);
    p->translate(rect.topLeft()); p->scale(rect.width() / 24.0, rect.height() / 24.0);
    const auto &t = PaleoTheme::tokens(); QPen pen(mode == QIcon::Disabled ? t.textDisabled : t.textMuted, 1.7);
    pen.setCapStyle(Qt::RoundCap); pen.setJoinStyle(Qt::RoundJoin); p->setPen(pen); p->setBrush(Qt::NoBrush);
    QPainterPath path;
    if (m_name.contains("Search")) {
      p->drawEllipse(QPointF(10, 10), 6, 6); p->drawLine(QPointF(15, 15), QPointF(20, 20));
    } else if (m_name.contains("Zoom")) {
      p->drawLine(QPointF(6, 12), QPointF(18, 12));
      if (m_name.endsWith("In")) p->drawLine(QPointF(12, 6), QPointF(12, 18));
    } else if (m_name.endsWith("Fit")) {
      path.moveTo(4, 9); path.lineTo(4, 4); path.lineTo(9, 4); path.moveTo(15, 4); path.lineTo(20, 4); path.lineTo(20, 9);
      path.moveTo(20, 15); path.lineTo(20, 20); path.lineTo(15, 20); path.moveTo(9, 20); path.lineTo(4, 20); path.lineTo(4, 15);
    } else if (m_name.endsWith("Refresh")) {
      p->drawArc(QRectF(5, 5, 14, 14), 40 * 16, 285 * 16); path.moveTo(20, 4); path.lineTo(20, 10); path.lineTo(14, 10);
    } else if (m_name.endsWith("Back")) {
      path.moveTo(11, 5); path.lineTo(4, 12); path.lineTo(11, 19); path.moveTo(4, 12); path.lineTo(20, 12);
    } else if (m_name.endsWith("Filters")) {
      path.moveTo(4, 5); path.lineTo(20, 5); path.lineTo(14, 12); path.lineTo(14, 19); path.lineTo(10, 21); path.lineTo(10, 12); path.closeSubpath();
    } else if (m_name.endsWith("Export")) {
      path.moveTo(12, 3); path.lineTo(12, 16); path.moveTo(7, 11); path.lineTo(12, 16); path.lineTo(17, 11);
      path.moveTo(4, 16); path.lineTo(4, 21); path.lineTo(20, 21); path.lineTo(20, 16);
    } else if (m_name.endsWith("Cancel")) {
      path.moveTo(6, 6); path.lineTo(18, 18); path.moveTo(6, 18); path.lineTo(18, 6);
    } else if (m_name.endsWith("History")) {
      p->drawEllipse(QPointF(12, 12), 8, 8); path.moveTo(12, 7); path.lineTo(12, 12); path.lineTo(16, 14);
    } else if (m_name.endsWith("RelationsToggle")) {
      p->drawEllipse(QPointF(12, 5), 2, 2); p->drawEllipse(QPointF(5, 18), 2, 2); p->drawEllipse(QPointF(19, 18), 2, 2);
      path.moveTo(11, 7); path.lineTo(6, 16); path.moveTo(13, 7); path.lineTo(18, 16);
    } else {
      for (int y : {6, 12, 18}) p->drawLine(QPointF(4, y), QPointF(20, y));
      p->setBrush(t.surface); for (const auto &center : {QPointF(9, 6), QPointF(16, 12), QPointF(8, 18)}) p->drawEllipse(center, 2, 2);
    }
    p->drawPath(path); p->restore();
  }
  QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
    QPixmap pm(size); pm.fill(Qt::transparent); QPainter painter(&pm); paint(&painter, pm.rect(), mode, state); return pm;
  }
  QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
    auto pm = pixmap(size * scale, mode, state); pm.setDevicePixelRatio(scale); return pm;
  }
private:
  QString m_name;
};
}
namespace PaleoTheme {
QIcon graphIcon(const QString &name) { return QIcon(new GraphIcon(name)); }
}
