// 层：视图
#pragma once

// 方向70（Unity 清障）：curvebrowser/depthruler/correlationpanel 匿名
// namespace 各带一份同构主题色取值（kTextMuted/kBorder），UNITY_BUILD 合批
// 即重定义。收拢单一定义；调用点经 `using` 零改动（色值单一真源仍是
// PaleoTheme tokens，这里只是惰性取值 helper）。

#include "../paleotheme.h"

#include <QColor>

namespace paleo::ui_detail {

inline QColor kTextMuted() { return PaleoTheme::tokens().textMuted; }
inline QColor kBorder() { return PaleoTheme::tokens().border; }

} // namespace paleo::ui_detail
