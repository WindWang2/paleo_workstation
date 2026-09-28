// 层：视图
#pragma once
#include <QIcon>
class QString;

// src/ui/paleoicons — ribbon 图标的单一出口。
//
//   能用 QGIS 的用 QGIS：vendor QGIS default 主题 svg 编进 qgis_gui 的
//   qrc（":/images/themes/default/"），与安装路径无关——qgisTheme() 取，
//   取不到返回空 QIcon（按钮退化为文本，行为不变）。
//   QGIS 没有对应语义的才自绘：QPainter 线稿，笔画用 DESIGN.md text
//   token，与 vendor 图标同色系不突兀。
//
//   暗色翻案（DESIGN.md 决策日志 2026-09-28）：QGIS default 主题是深
//   glyph，暗底不可见——qgisTheme() 在暗色主题下经 tintForDarkTheme()
//   逐像素提亮（Plus 合成，透明区不动）；自绘图标的墨色随主题翻。已
//   构造的图标不回填（切主题后新取的图标带新色），完整刷新随重启。
namespace PaleoIcons
{
  // name 形如 "mActionSelectRectangle.svg"（不带前导斜杠）。
  QIcon qgisTheme( const QString &name );

  // 深色 glyph → 浅色的提亮变换（独立暴露供测试/复用）。
  QIcon tintForDarkTheme( const QIcon &icon );

  QIcon maximize(); // 预览最大化：单一方框
  QIcon restore();  // 还原预览：叠放双方框
} // namespace PaleoIcons
