// 层：数据
#pragma once

// 方向70（Unity 清障）：inversion 族 .cpp 匿名 namespace 各带一份同构
// kNan（quiet_NaN float 常量），UNITY_BUILD 合批即重定义。收拢单一定义；
// 各 .cpp 去本地定义后，unqualified kNan 经 paleo::inversion 命名空间命中
//（匿名 namespace 嵌套在内，查找先命中本定义，调用点零改动）。

#include <limits>

namespace paleo::inversion {

inline constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

} // namespace paleo::inversion
