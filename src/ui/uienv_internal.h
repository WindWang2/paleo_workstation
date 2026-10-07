// 层：视图
#pragma once

// 方向70（Unity 清障）：paleomainwindow/layerpropertiesdialog 匿名 namespace
// 各带一份同构 isOffscreen（平台名判定），UNITY_BUILD 合批即重定义。
// 收拢单一定义；调用点经 `using paleo::ui_detail::isOffscreen;` 零改动。

#include <QGuiApplication>
#include <QString>

namespace paleo::ui_detail {

/// offscreen 平台判定（CI/无头档跳过窗口暴露等待的口径）。
inline bool isOffscreen()
{
    return QGuiApplication::platformName() == QLatin1String("offscreen");
}

} // namespace paleo::ui_detail
