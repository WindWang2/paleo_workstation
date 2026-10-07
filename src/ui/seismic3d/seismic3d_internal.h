// 层：视图
#pragma once

// 方向70（Unity 清障）：seismic3d 渲染族 .cpp 匿名 namespace 各带一份同构
// 归一化/量化 helper（Normalize/NormalizeF/ToByte），UNITY_BUILD 合批即
// 重定义。收拢单一定义；各 .cpp 代码本就嵌套在 namespace seismic 内，
// 去本地定义后 unqualified 调用直接命中，调用点零改动。

#include <algorithm>
#include <cmath>

namespace seismic {

/// 体值 → [-scale/2, scale/2] 归一（int 源）。
inline float Normalize(int value, int minValue, int maxValue, float scale)
{
    const float range = static_cast<float>(std::max(1, maxValue - minValue));
    return ((static_cast<float>(value - minValue) / range) - 0.5f) * scale;
}

/// 体值 → [-scale/2, scale/2] 归一（float 源）。
inline float NormalizeF(float value, int minValue, int maxValue, float scale)
{
    const float range = static_cast<float>(std::max(1, maxValue - minValue));
    return ((value - static_cast<float>(minValue)) / range - 0.5f) * scale;
}

/// [0,1] → 0..255 钳制量化。
inline unsigned char ToByte(float v)
{
    const int val = static_cast<int>(std::round(v * 255.0f));
    return static_cast<unsigned char>(std::clamp(val, 0, 255));
}

} // namespace seismic
