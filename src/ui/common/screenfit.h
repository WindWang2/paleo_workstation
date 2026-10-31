#pragma once
// 层：视图
// #854/#887：对话框默认尺寸不超过所在屏幕可用区的 90%（小屏/高缩放下底部按钮
// 不会落到屏幕外）；大屏上与原默认值一致。
// 单一实现在 ui/windowsizing_internal.h（paleo::ui_detail）；此处仅为调用点便利的转发，
// 不再维护第二套口径（R7 #887/#888）。
#include "ui/windowsizing_internal.h"

namespace paleo::ui {
inline QSize boundedToScreen(const QSize &want, const QScreen *screen)
{
  return screen ? ui_detail::boundedToAvailable(want, screen->availableGeometry().size()) : want;
}
inline void resizeWithinScreen(QWidget *w, int width, int height)
{
  ui_detail::resizeWithinScreen(w, QSize(width, height));
}
} // namespace paleo::ui
