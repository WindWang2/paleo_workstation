#pragma once
// 层：视图
// R6 (#846/#847/#850)：替代 setFixedWidth(px) 的“至少 px、且不小于内容”宽度。
// Fixed 水平策略 + 显式最小宽 => 实际宽 = max(px, sizeHint)，值域/后缀/字体
// 变化后 sizeHint 随之更新，不再截断数字；外观在默认字体下与原固定宽一致。
#include <QSizePolicy>
#include <QWidget>

namespace paleo::ui {
inline void fitWidthAtLeast(QWidget *w, int px)
{
  if (!w)
    return;
  w->setMinimumWidth(px);
  w->setMaximumWidth(QWIDGETSIZE_MAX);
  w->setSizePolicy(QSizePolicy::Fixed, w->sizePolicy().verticalPolicy());
}
} // namespace paleo::ui
