// 层：视图
#pragma once

// 方向70（Unity 清障）：previewprofilepanel/previewhistogramwidget 匿名
// namespace 各带一份同构 mono8（labelPt 号等宽字体），UNITY_BUILD 合批即
// 重定义。收拢单一定义；调用点经 `using paleo::ui_detail::mono8;` 零改动。
//（datapreviewtabseismic 的同名 mono8 是函数内局部变量，不受影响。）

#include "../paleotheme.h"

#include <QFont>

namespace paleo::ui_detail {

/// 等宽字体 + labelPt 字号（数据预览读数/刻度口径）。
inline QFont mono8()
{
  QFont f = PaleoTheme::monoFont();
  f.setPointSize(PaleoTheme::tokens().labelPt);
  return f;
}

} // namespace paleo::ui_detail
