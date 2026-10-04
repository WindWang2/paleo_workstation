// 层：视图
#pragma once
#include <QIcon>
class QString;
class QColor;

// src/ui/paleoicons — ribbon 图标的单一出口。
//
//   能用 QGIS 的用 QGIS：vendor QGIS default 主题 svg 编进 qgis_gui 的
//   qrc（":/images/themes/default/"），与安装路径无关——qgisTheme() 取，
//   取不到返回空 QIcon（按钮退化为文本，行为不变）。
//   QGIS 没有对应语义的才自绘：QPainter 线稿，笔画用 DESIGN.md text
//   token，与 vendor 图标同色系不突兀。
//
//   QIconEngine 在绘制时读取当前 token；已存在的按钮实时跟随主题。
//   SVG 的形状、透明边缘与上游留白保持，HiDPI 按目标 DPR 渲染。
namespace PaleoIcons
{
  // name 形如 "mActionSelectRectangle.svg"（不带前导斜杠）。
  QIcon qgisTheme( const QString &name );
  // QGIS 元数据提供的原生动作图标也经同一主题出口。
  QIcon themed( const QIcon &icon );

  // 深色 glyph → 浅色的提亮变换（独立暴露供测试/复用）。
  QIcon tintForDarkTheme( const QIcon &icon );

  // QGIS 9pt 工具条的既定 glyph 档位，统一四处动作按钮入口。
  QSize toolbarSize();
  QIcon inactive(); // 未激活层指示点
  QIcon dataLine(const QColor &color); // 曲线数据色样；不改变原色
  QIcon maximize(); // 预览最大化：单一方框
  QIcon restore();  // 还原预览：叠放双方框
} // namespace PaleoIcons
