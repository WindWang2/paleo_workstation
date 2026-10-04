// 层：视图
// token 例外：DESIGN 数据符号例外：透明度色带使用固定灰白棋盘作合成参照，灰色为无停靠点的插值值。（tools/ui-token-exceptions.json 精确计数）。
#include "seismic3dtfeditor.h"

#include "../paleotheme.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace seismic {

Seismic3DTfEditorWidget::Seismic3DTfEditorWidget(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumSize(300, 150);
    if (tf_.stops().isEmpty())
        tf_ = Seismic3DTransferFunction::preset(QStringLiteral("均匀半透明"));
}

void Seismic3DTfEditorWidget::setTransferFunction(const Seismic3DTransferFunction &tf)
{
    tf_ = tf;
    update();
}

QRect Seismic3DTfEditorWidget::gradientRect() const
{
    // 布局：上 2/3 不透明度曲线区，下 1/3 色带；左右各留 16px 边距（停靠点
    // 圆半径 6px 的可拖空间）
    const int m = 16;
    const int gradH = std::max(28, height() / 3);
    return QRect(m, height() - m - gradH, width() - 2 * m, gradH);
}

QRect Seismic3DTfEditorWidget::curveRect() const
{
    const int m = 16;
    const QRect grad = gradientRect();
    return QRect(m, m, width() - 2 * m, grad.top() - 2 * m);
}

int Seismic3DTfEditorWidget::hitStop(const QPoint &pos) const
{
    const QRect cr = curveRect();
    for (int i = tf_.stops().size() - 1; i >= 0; --i)
    {
        const QPointF sp((cr.left() + tf_.stops()[i].pos * cr.width()),
                     (cr.bottom() - tf_.stops()[i].alpha * cr.height()));
        if (std::hypot(sp.x() - pos.x(), sp.y() - pos.y()) <= 9.0)
            return i;
    }
    return -1;
}

QPointF Seismic3DTfEditorWidget::stopToPoint(int index) const
{
    const QRect cr = curveRect();
    const auto &stop = tf_.stops()[index];
    return QPointF(cr.left() + stop.pos * cr.width(),
                   cr.bottom() - stop.alpha * cr.height());
}

QColor Seismic3DTfEditorWidget::interpolatedColor(float pos) const
{
    const auto &stops = tf_.stops();
    if (stops.isEmpty())
        return QColor(128, 128, 128);
    if (pos <= stops.first().pos)
        return QColor(stops.first().color);
    if (pos >= stops.last().pos)
        return QColor(stops.last().color);
    int k = 0;
    while (k + 2 < stops.size() && stops[k + 1].pos < pos)
        ++k;
    const float span = std::max(1e-6f, stops[k + 1].pos - stops[k].pos);
    const float t = std::clamp((pos - stops[k].pos) / span, 0.0f, 1.0f);
    const QColor a(stops[k].color), b(stops[k + 1].color);
    return QColor(a.redF() + t * (b.redF() - a.redF()),
                  a.greenF() + t * (b.greenF() - a.greenF()),
                  a.blueF() + t * (b.blueF() - a.blueF()));
}

void Seismic3DTfEditorWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter p(this);
    const auto &t = PaleoTheme::tokens();

    p.fillRect(rect(), t.surface);

    const QRect grad = gradientRect();
    const QRect cr = curveRect();

    // 色带：底棋盘（不透明度可视化）+ 停靠点渐变 ×alpha 合成
    const int cell = 8;
    for (int x = 0; x < grad.width(); x += cell)
    {
        for (int y = 0; y < grad.height(); y += cell)
        {
            const bool light = ((x / cell) + (y / cell)) % 2 == 0;
            p.fillRect(grad.x() + x, grad.y() + y, std::min(cell, grad.width() - x),
                       std::min(cell, grad.height() - y),
                       light ? QColor(255, 255, 255) : QColor(224, 224, 224));
        }
    }
    for (int x = 0; x < grad.width(); ++x)
    {
        const float pos = float(x) / float(std::max(1, grad.width() - 1));
        QColor c = interpolatedColor(pos);
        // 该位置的合成不透明度（曲线插值）
        float alpha = 0.0f;
        const auto &stops = tf_.stops();
        if (pos <= stops.first().pos)
            alpha = stops.first().alpha;
        else if (pos >= stops.last().pos)
            alpha = stops.last().alpha;
        else
        {
            int k = 0;
            while (k + 2 < stops.size() && stops[k + 1].pos < pos)
                ++k;
            const float span = std::max(1e-6f, stops[k + 1].pos - stops[k].pos);
            const float tt = std::clamp((pos - stops[k].pos) / span, 0.0f, 1.0f);
            alpha = stops[k].alpha + tt * (stops[k + 1].alpha - stops[k].alpha);
        }
        c.setAlpha(int(alpha * 255.0f + 0.5f));
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRect(grad.x() + x, grad.y(), 1, grad.height());
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(t.border, 1));
    p.drawRect(grad.adjusted(0, 0, -1, -1));

    // 曲线区：网格 + 不透明度折线（=1 顶线参考虚线）
    p.setPen(QPen(t.border, 1, Qt::DotLine));
    p.drawLine(cr.left(), cr.top(), cr.right(), cr.top());
    p.setPen(QPen(t.textDisabled, 1, Qt::DotLine));
    p.drawLine(cr.left(), cr.bottom(), cr.right(), cr.bottom());
    p.setPen(QPen(t.border, 1));
    p.drawRect(cr.adjusted(0, 0, -1, -1));

    // 曲线下方用色带色的半透明填充（低不透明度=更透）
    QPainterPath fillPath;
    fillPath.moveTo(cr.left(), cr.bottom());
    for (int i = 0; i < tf_.stops().size(); ++i)
        fillPath.lineTo(stopToPoint(i));
    fillPath.lineTo(cr.right(), cr.bottom());
    for (int x = 0; x < cr.width(); x += 4)
    {
        const float pos = float(x) / float(std::max(1, cr.width() - 1));
        p.fillRect(QRect(cr.x() + x, cr.y(), 4, cr.height()),
                   QBrush(interpolatedColor(pos).darker(220)));
    }
    p.setPen(Qt::NoPen);
    p.setOpacity(0.35);
    p.drawPath(fillPath);
    p.setOpacity(1.0);

    // 折线 + 停靠点（悬停/拖拽 = primary 高亮，其余 text 色；选中 chip 语义）
    QPolygonF poly;
    for (int i = 0; i < tf_.stops().size(); ++i)
        poly << stopToPoint(i);
    p.setPen(QPen(t.text, 2));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawPolyline(poly);
    for (int i = 0; i < tf_.stops().size(); ++i)
    {
        const bool active = (i == dragStop_ || i == hoverStop_);
        const QPointF sp = stopToPoint(i);
        p.setPen(QPen(active ? t.primary : t.text, active ? 2 : 1));
        p.setBrush(interpolatedColor(tf_.stops()[i].pos));
        p.drawEllipse(QRectF(sp.x() - (active ? 6.0 : 4.5),
                             sp.y() - (active ? 6.0 : 4.5),
                             (active ? 6.0 : 4.5) * 2, (active ? 6.0 : 4.5) * 2));
    }

    // 角标：0=负峰 / 1=正峰（label 8pt、muted）
    QFont f = PaleoTheme::monoFont();
    f.setPointSize(PaleoTheme::tokens().labelPt);
    p.setFont(f);
    p.setPen(QPen(t.textMuted, 1));
    p.drawText(cr.adjusted(2, 0, 0, 0), Qt::AlignLeft | Qt::AlignTop,
               QStringLiteral("1.0"));
    p.drawText(cr.adjusted(2, 0, 0, -2), Qt::AlignLeft | Qt::AlignBottom,
               QStringLiteral("0.0"));
    p.drawText(grad.adjusted(2, 0, -2, 0), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("负峰"));
    p.drawText(grad.adjusted(2, 0, -2, 0), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("正峰"));
}

void Seismic3DTfEditorWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        const int hit = hitStop(event->position().toPoint());
        if (hit >= 0)
        {
            dragStop_ = hit;
            update();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void Seismic3DTfEditorWidget::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    if (dragStop_ >= 0)
    {
        const QRect cr = curveRect();
        const float posF = std::clamp(float(pos.x() - cr.left()) / float(std::max(1, cr.width())), 0.0f, 1.0f);
        const float alphaF = std::clamp(float(cr.bottom() - pos.y()) / float(std::max(1, cr.height())), 0.0f, 1.0f);
        tf_.setStopPos(dragStop_, posF);
        tf_.setStopAlpha(dragStop_, alphaF);
        emitChanged();
        return;
    }
    const int hit = hitStop(pos);
    if (hit != hoverStop_)
    {
        hoverStop_ = hit;
        setCursor(hit >= 0 ? Qt::SizeAllCursor : Qt::ArrowCursor);
        update();
    }
}

void Seismic3DTfEditorWidget::mouseReleaseEvent(QMouseEvent * /*event*/)
{
    if (dragStop_ >= 0)
    {
        dragStop_ = -1;
        update();
    }
}

void Seismic3DTfEditorWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    const QRect cr = curveRect();
    if (!cr.adjusted(-6, -9, 6, 9).contains(pos))
        return;
    if (hitStop(pos) >= 0)
        return; // 已有停靠点：不叠加
    const float posF = std::clamp(float(pos.x() - cr.left()) / float(std::max(1, cr.width())), 0.0f, 1.0f);
    const float alphaF = std::clamp(float(cr.bottom() - pos.y()) / float(std::max(1, cr.height())), 0.0f, 1.0f);
    tf_.addStop(posF, interpolatedColor(posF).rgba(), alphaF);
    emitChanged();
}

void Seismic3DTfEditorWidget::contextMenuEvent(QContextMenuEvent *event)
{
    const int hit = hitStop(event->pos());
    if (hit < 0)
    {
        QWidget::contextMenuEvent(event);
        return;
    }
    QMenu menu(this);
    QAction *remove = menu.addAction(tr("删除停靠点"));
    // 端点删除会丢失负峰/正峰锚——两端不允许删
    remove->setEnabled(hit > 0 && hit < tf_.stops().size() - 1 && tf_.stops().size() > 2);
    QAction *chosen = menu.exec(event->globalPos());
    if (chosen == remove)
    {
        tf_.removeStop(hit);
        hoverStop_ = -1;
        emitChanged();
    }
}

void Seismic3DTfEditorWidget::emitChanged()
{
    update();
    emit transferFunctionChanged(tf_);
}

} // namespace seismic
