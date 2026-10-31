#pragma once
// 层：视图
// #854：对话框默认尺寸不超过所在屏幕可用区的 90%（小屏/高缩放下底部按钮
// 不会落到屏幕外）；大屏上与原固定默认值一致。
#include <QScreen>
#include <QSize>
#include <QWidget>

namespace paleo::ui {
inline QSize boundedToScreen(const QSize &want, const QScreen *screen)
{
  if (!screen)
    return want;
  return want.boundedTo(screen->availableGeometry().size() * 0.9);
}
inline void resizeWithinScreen(QWidget *w, int width, int height)
{
  const QWidget *anchor = w->parentWidget() ? w->parentWidget()->window() : w;
  w->resize(boundedToScreen({width, height}, anchor->screen()));
}
} // namespace paleo::ui
