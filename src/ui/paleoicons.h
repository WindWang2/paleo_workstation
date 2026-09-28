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
//   token（#24303E），与 vendor 图标同色系不突兀。
namespace PaleoIcons
{
  // name 形如 "mActionSelectRectangle.svg"（不带前导斜杠）。
  QIcon qgisTheme( const QString &name );

  QIcon maximize(); // 预览最大化：单一方框
  QIcon restore();  // 还原预览：叠放双方框
} // namespace PaleoIcons
