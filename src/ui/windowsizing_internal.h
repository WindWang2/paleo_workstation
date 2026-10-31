// 层：视图
#pragma once

// R6 #835/#844：窗口/对话框尺寸按「所在屏」约束的单一口径。
// - 主窗口不设最小尺寸（用户 2026-10-31「不要限制」，取代 PLAN §42.11）；首次启动按所在屏 90% 起步。
// - 对话框默认尺寸：preferred.boundedTo(0.9×可用区)，与 paleostartupscreen 同口径。

#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QWidget>

namespace paleo::ui_detail {

/// 期望尺寸按可用区 frac 收紧（avail 无效时原样返回）。
inline QSize boundedToAvailable(const QSize &preferred, const QSize &avail, double frac = 0.9)
{
    if (avail.width() <= 0 || avail.height() <= 0)
        return preferred;
    return preferred.boundedTo(QSize(qRound(avail.width() * frac), qRound(avail.height() * frac)));
}

/// 控件所在屏（未显示时退回 primaryScreen）的可用区尺寸。
inline QSize availableSizeFor(const QWidget *w)
{
    const QScreen *screen = w ? w->screen() : nullptr;
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    return screen ? screen->availableGeometry().size() : QSize();
}

/// 对话框默认尺寸：resize(preferred) 但不超出所在屏 90%；同时把过大的最小尺寸一并收紧。
inline void resizeWithinScreen(QWidget *w, const QSize &preferred)
{
    if (!w)
        return;
    const QSize avail = availableSizeFor(w);
    if (w->minimumWidth() > 0 || w->minimumHeight() > 0)
        w->setMinimumSize(boundedToAvailable(w->minimumSize(), avail));
    w->resize(boundedToAvailable(preferred, avail));
}

} // namespace paleo::ui_detail

#include <QAbstractScrollArea>
#include <QFontMetrics>

namespace paleo::ui_detail {

/// 列表/文本框 n 行内容的高度（按字体行距 + 边框，随字体/DPI 缩放；R6 #836/#842）。
inline int lineRows(const QWidget *w, int rows)
{
    const QFontMetrics fm = w ? w->fontMetrics() : QFontMetrics(QFont());
    int frame = 4;
    if (const auto *area = qobject_cast<const QAbstractScrollArea *>(w))
        frame = 2 * area->frameWidth() + 4;
    return fm.lineSpacing() * rows + frame;
}

} // namespace paleo::ui_detail

#include <QHeaderView>
#include <QTableView>

namespace paleo::ui_detail {

/// 表格「贴合可见行」的高度：表头 + 实际行高之和（跳过隐藏行）+ 边框（R6 #842 单一口径）。
/// minRows>0 时至少容纳 minRows 行默认行高（空表也不塌成一条线）。
inline int tableHeightForRows(const QTableView *t, int minRows = 0)
{
    if (!t)
        return 0;
    const QHeaderView *hh = t->horizontalHeader();
    int h = 2 * t->frameWidth();
    if (hh && !hh->isHidden())
        h += qMax(hh->height(), hh->sizeHint().height());
    int rows = 0, body = 0;
    const int n = t->model() ? t->model()->rowCount() : 0;
    for (int r = 0; r < n; ++r)
        if (!t->isRowHidden(r)) { body += t->rowHeight(r); ++rows; }
    if (rows < minRows)
        body += (minRows - rows) * t->verticalHeader()->defaultSectionSize();
    return h + body;
}

} // namespace paleo::ui_detail

namespace paleo::ui_detail {

/// n 个平均字符宽（随字体缩放；替代裸像素 setMaximumWidth，R6 #845）。
inline int charColumns(const QWidget *w, int chars)
{
    const QFontMetrics fm = w ? w->fontMetrics() : QFontMetrics(QFont());
    return fm.averageCharWidth() * chars;
}

} // namespace paleo::ui_detail
