// 层：视图
#pragma once

class SARibbonBar;
class SARibbonCategory;
class SARibbonPanel;

// src/ui/ribboncollapse — Office 式 ribbon 分组自适应折叠（#860）。
//
// 每个页签（SARibbonCategory）挂一个折叠器：页签宽度不够时，按「优先级低→高、
// 同优先级自右向左」逐组降级，一轮降一级、全部组都降过再进下一级：
//   0 Full     原样（大按钮 + 小按钮）
//   1 Small    大按钮改小按钮（图标 + 文字，一行）
//   2 IconOnly 小按钮只留图标（tooltip / accessibleName 仍是完整文案）
//   3 Popup    整组收成一颗下拉钮（组图标 + 组名 ▾），点开弹出原组的全部控件
// 空间恢复时按相反顺序展开。全部组都到 3 级仍放不下，才交给 SARibbon 的滚动箭头。
//
// 组的折叠优先级：panel 动态属性 "paleoCollapsePriority"（int，缺省 0，越小越先折）。
// 当前级别：panel 动态属性 "paleoCollapseLevel"（int）。
namespace PaleoRibbon
{
  enum CollapseLevel
  {
    CollapseFull = 0,
    CollapseSmall = 1,
    CollapseIconOnly = 2,
    CollapsePopup = 3
  };

  // 给 bar 的每个页签装折叠器（幂等，新加的页签/组会被自动接管）。
  void enableAdaptiveCollapse(SARibbonBar *bar);

  // 立即按当前宽度重算一次（测试 / 构建完成后调用；正常运行靠 Resize/Show 驱动）。
  void relayoutCollapse(SARibbonCategory *category);

  // 组当前级别；组未被接管时为 CollapseFull。
  int collapseLevel(const SARibbonPanel *panel);

  // 3 级时占位的下拉组（其余级别返回 nullptr）。
  SARibbonPanel *collapsedStub(const SARibbonPanel *panel);
} // namespace PaleoRibbon
