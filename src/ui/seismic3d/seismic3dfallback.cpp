// 层：视图
#include "seismic3dfallback.h"

#include <QPainter>
#include "../paleotheme.h"

#include <array>

namespace seismic {

Seismic3DFallbackWidget::Seismic3DFallbackWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(400, 260);
}

void Seismic3DFallbackWidget::setSlice(int slot, const QImage &image, const QString &label)
{
    if (slot < 0 || slot > 2)
        return;
    slices_[static_cast<std::size_t>(slot)] = image;
    labels_[static_cast<std::size_t>(slot)] = label;
    update();
}

void Seismic3DFallbackWidget::clearSlices()
{
    slices_.fill(QImage());
    labels_.fill(QString());
    update();
}

void Seismic3DFallbackWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), PaleoTheme::tokens().surfaceAlt);

    QFont head = PaleoTheme::bodyFont();
    head.setBold(true);
    QFont cap = PaleoTheme::bodyFont();
    cap.setPointSize(PaleoTheme::tokens().labelPt);

    // 顶部说明条
    p.setFont(head);
    p.setPen(PaleoTheme::tokens().warningText);
    p.drawText(QRect(8, 6, width() - 16, 22), Qt::AlignLeft | Qt::AlignVCenter,
               tr("三维视口不可用（OpenGL 上下文创建失败）——已切换 2D 切片拼接视图"));
    p.setFont(cap);
    p.setPen(PaleoTheme::tokens().textMuted);
    p.drawText(QRect(8, 28, width() - 16, 16), Qt::AlignLeft,
               tr("如需完整三维交互，请检查显卡驱动或设置 QT_OPENGL=software 后重启。"));

    const int top = 50;
    const int gap = 8;
    const int cellW = (width() - gap * 4) / 3;
    const int cellH = height() - top - gap;
    const char *names[3] = {QT_TRANSLATE_NOOP("seismic::Seismic3DFallbackWidget", "纵测线 IL"),
                            QT_TRANSLATE_NOOP("seismic::Seismic3DFallbackWidget", "横测线 XL"),
                            QT_TRANSLATE_NOOP("seismic::Seismic3DFallbackWidget", "时间切片 Z")};
    for (int i = 0; i < 3; ++i)
    {
        const QRect cell(gap + i * (cellW + gap), top, cellW, cellH);
        p.fillRect(cell, PaleoTheme::tokens().surface);
        p.setPen(PaleoTheme::tokens().border);
        p.drawRect(cell);

        const QImage &img = slices_[static_cast<std::size_t>(i)];
        if (img.isNull())
        {
            p.setPen(PaleoTheme::tokens().textMuted);
            p.drawText(cell, Qt::AlignCenter, tr("切片加载中…"));
        }
        else
        {
            const QSizeF scaled = img.size().scaled(cell.size(), Qt::KeepAspectRatio);
            const QRectF dst(cell.left() + (cell.width() - scaled.width()) / 2.0,
                             cell.top() + (cell.height() - scaled.height()) / 2.0,
                             scaled.width(), scaled.height());
            p.drawImage(dst, img);
        }
        p.setPen(PaleoTheme::tokens().textMuted);
        p.setFont(cap);
        const QString label = labels_[static_cast<std::size_t>(i)].isEmpty()
            ? tr(names[i])
            : labels_[static_cast<std::size_t>(i)];
        p.drawText(QRect(cell.left() + 4, cell.top() + 4, cell.width() - 8, 14),
                   Qt::AlignLeft | Qt::AlignVCenter, label);
    }
}

} // namespace seismic
