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
// （PW-096 起）全量 qHashBits values（NaN 归一为 -99999.0，保证 NaN 图与 0 值图不
// 撞键）；哈希状态用无符号（回绕良定义，有符号乘法溢出是 UB），取位用 memcpy
// （无 strict-aliasing 违规）。
// 显示参数（gain / contrast / threshold / 极性 / AGC / 增益曲线 / colormap /
// 反转）由调用方另行并入键——参数变了键必变，缓存不会回吐旧观感。
// 拆分不改键的任何一个因子；改键即改缓存命中面，属行为变更。
#include "ui/seismicsection/seismicsectioncanvas.h"

#include <cstddef>
#include <cstring>
#include <cmath>
#include <QHashFunctions>

namespace seismic {

// D2.1 纹理缓存键：内容指纹（值域 + 全值 FNV）× 显示参数
// 哈希状态用无符号（回绕良定义；有符号乘法溢出是 UB），取位用 memcpy（无 strict-aliasing 违规）。
inline qint64 SliceFingerprint(const SgySliceImage &img) {
    // PW-096：全内容哈希（不再 4096 抽样——抽样相同、未采样值不同的两条线会
    // 撞键回吐旧图）。qHashBits 按字节，NaN 位型自然与 0 区分。
    const size_t h = qHashMulti(size_t(0x9e3779b97f4a7c15ull), img.width, img.height,
                                img.valueMin, img.valueMax,
                                qHashBits(img.values.data(), img.values.size() * sizeof(float)));
    return static_cast<qint64>(h);
}

// 垂直剖面入图行序转换（显示真值链的视图边界约定）。
// SgySliceImage 的行序契约 = vendor 提取契约：行 0 = 最深样/最大 TWT
// （SgyVolume.cpp ExtractSlice 的 row = sampleCount-1-s；属性扫描/任意线/
// 引擎 Slice2D 全线同契约）。画布的整套时间映射——标尺 timeToPixelY、
// hover/点击的 sampleIdx=round((twt-t0)/dt)、拾取/井分层/合成记录按物理
// TWT 锚定——全部是「行 y = 采样 y 升序」。两侧在视图边界统一翻转，
// 画布内部空间全链升序；时间切片（行 = IL 轴，无时间映射）不经本函数。
// rgba 与 values 同几何（vendor FinishImage 烘焙），一并翻转保持一致。
inline SgySliceImage ToAscendingSampleRows(const SgySliceImage &img) {
    if (img.width <= 0 || img.height <= 0 ||
        img.values.size() < std::size_t(img.width) * std::size_t(img.height))
        return img;
    SgySliceImage out = img;
    for (int y = 0; y < img.height; ++y) {
        const float *src = img.values.data() +
                           std::size_t(img.height - 1 - y) * std::size_t(img.width);
        float *dst = out.values.data() + std::size_t(y) * std::size_t(img.width);
        std::memcpy(dst, src, sizeof(float) * std::size_t(img.width));
    }
    if (img.rgba.size() == img.values.size() * 4) {
        out.rgba.resize(img.rgba.size());
        for (int y = 0; y < img.height; ++y) {
            const unsigned char *src = img.rgba.data() +
                                       std::size_t(img.height - 1 - y) *
                                           std::size_t(img.width) * 4;
            unsigned char *dst = out.rgba.data() +
                                 std::size_t(y) * std::size_t(img.width) * 4;
            std::memcpy(dst, src, std::size_t(img.width) * 4);
        }
    }
    return out;
}

// D2.1 键的显示参数因子：增益曲线状态指纹（控制点数 + 各点 TWT/增益的
// FNV-1a）。空曲线 = 0——曲线有无/内容变化必变键，不同曲线烘焙的缓存
// 图不会互相回吐。
inline qint64 GainCurveFingerprint(const std::vector<SectionGainNode> &nodes) {
    if (nodes.empty())
        return 0;
    quint64 h = 1469598103934665603ull;
    const auto mix = [&h](double v) {
        quint64 bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        h ^= bits;
        h *= 1099511628211ull;
    };
    mix(static_cast<double>(nodes.size()));
    for (const SectionGainNode &n : nodes) {
        mix(n.twtMs);
        mix(n.gain);
    }
    return static_cast<qint64>(h);
}

// PW-096：纹理缓存键 = qHashMulti(内容指纹, 几何, t0/dt, 全部显示参数)——
// 不再用有符号移位 XOR（溢出 UB、参数间位重叠撞键）。
inline qint64 TextureCacheKey(const SgySliceImage &img, int traces, int samples,
                              double t0Ms, double dtMs, double gain, double contrast,
                              int colorMap, bool cmapInverted, double threshold,
                              bool polarityInverted, int agcWindowMs, qint64 gainCurveFp) {
    return static_cast<qint64>(qHashMulti(size_t(SliceFingerprint(img)), traces, samples,
                                          t0Ms, dtMs, gain, contrast, colorMap, cmapInverted,
                                          threshold, polarityInverted, agcWindowMs,
                                          gainCurveFp));
}

} // namespace seismic
