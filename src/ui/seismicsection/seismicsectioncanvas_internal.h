// 层：视图
#pragma once
// 方向 65：画布拆分后的内部契约头。
//
// 唯一共享项是 D2.1 纹理缓存键的「内容指纹」 SliceFingerprint——它被主体 TU
// 的 setSectionData（入队前算键）与渲染核 TU 的 rebuildImage（重建后算键）
// 两侧使用，故提出为头；colormap 采样 / LOD / wiggle 等 helper 经实测均为单
// TU 局部，留在各自 TU 的匿名命名空间内，不进本头。
// 函数体 inline，多 TU include 无 ODR 风险。
//
// 【D2.1 纹理缓存键语义（消费面引用点，方向 43 attr-volume 依赖）】
// 键 = 内容指纹 × 显示参数。内容指纹 = FNV-1a：先混入 valueMin / valueMax，再
// 按 ≥4096 抽样的步长遍历 values（NaN 归一为 -99999.0，保证 NaN 图与 0 值图不
// 撞键）；哈希状态用无符号（回绕良定义，有符号乘法溢出是 UB），取位用 memcpy
// （无 strict-aliasing 违规）。
// 显示参数（gain / contrast / threshold / 极性 / AGC / 增益曲线 / colormap /
// 反转）由调用方另行并入键——参数变了键必变，缓存不会回吐旧观感。
// 拆分不改键的任何一个因子；改键即改缓存命中面，属行为变更。
#include "ui/seismicsection/seismicsectioncanvas.h"

#include <cstddef>
#include <cstring>
#include <cmath>

namespace seismic {

// D2.1 纹理缓存键：内容指纹（值域 + 全值 FNV）× 显示参数
// 哈希状态用无符号（回绕良定义；有符号乘法溢出是 UB），取位用 memcpy（无 strict-aliasing 违规）。
inline qint64 SliceFingerprint(const SgySliceImage &img) {
    quint64 h = 1469598103934665603ull;
    const auto mix = [&h](double v) {
        quint64 bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        h ^= bits;
        h *= 1099511628211ull;
    };
    mix(img.valueMin);
    mix(img.valueMax);
    const std::size_t stride = std::max<std::size_t>(1, img.values.size() / 4096);
    for (std::size_t i = 0; i < img.values.size(); i += stride) {
        const double v = img.values[i];
        mix(std::isnan(v) ? -99999.0 : static_cast<double>(v));
    }
    return static_cast<qint64>(h);
}

} // namespace seismic
