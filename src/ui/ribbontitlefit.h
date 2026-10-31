#pragma once
// 层：视图
// #860 续：标题行（页签同一行）的「页签优先」自适应。紧凑样式下 SARibbon 把右侧
// 按钮组按 sizeHint 摆好，剩下的才给页签——窄窗 / 高缩放时页签被整排挤没。
// 这里按需逐级让位（宽度全部由字体度量推导，不写死像素）：
//   1. 搜索框从首选宽度收窄到最小宽度；
//   2. 文字+图标的全局按钮改纯图标；
//   3. 搜索框收成放大镜图标钮，点击（或 Ctrl+K）在标题行内展开，失焦即收回；
//   4. 右侧按钮自右向左折进「更多」菜单。
// 空间回来时按相反顺序恢复。PALEO_RIBBON_COLLAPSE=0 同样关掉本功能。
class SARibbonBar;

namespace PaleoRibbon
{
  // 挂到 bar（幂等）。右侧组里 objectName 为 locatorSlot 的部件被视为搜索槽。
  void enableTitleRowFit(SARibbonBar *bar);
  // 搜索框已收成图标时把它展开并聚焦；未收起时什么都不做。返回是否展开了。
  bool revealTitleSearch(SARibbonBar *bar);
  // 立即按当前宽度重新适配（测试 / 外部改动后调用）。
  void refitTitleRow(SARibbonBar *bar);
} // namespace PaleoRibbon
