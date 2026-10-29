// 层：视图
#pragma once

// 地震振幅色标预设 + LUT 采样——2D 剖面画布（D2.8）与连井剖面井间缝
// 共用同一实现（2026-10-11 用户要求井间缝地震与剖面色标观感一致；
// 井间缝原先自写的非对称红蓝 LUT 废弃）。

#include "../seismic3d/seismic3d_internal.h" // ToByte

#include <QRgb>

#include <algorithm>
#include <cmath>

namespace seismic {

enum class SectionColorMapType {
    RedWhiteBlue = 0,   // Standard bipolar seismic (Peak=Red, Trough=Blue, Zero=White)
    Grayscale = 1,      // Black/White density
    Rainbow = 2,        // Spectrum
    // ---- D2.8：预设扩到 8 档 ----
    BlueWhiteRed = 3,   // 极性反转观感（Peak=Blue）
    BlackWhiteBlue = 4, // 经典纸面（Peak=Black, Trough=Blue）
    RedWhiteBlack = 5,  // 经典纸面镜像
    GreenWhiteMagenta = 6,
    CyanWhiteOrange = 7
};

constexpr int kSectionColorMapCount = 8;

// finding-150：零幅值锚定白色的对称色带——k=1（零幅值）→ (255,255,255)，
// k=0（满幅）→ 端色 (r,g,b)。旧实现把三通道同乘 k：零幅值端反而落在端色、
// 满幅端落黑，零点两侧颜色硬跳变（零幅值读不出「无信号」的白）。
inline QRgb WhiteAnchoredRamp(float k, float r, float g, float b) {
    return qRgba(ToByte(r + (1.0f - r) * k), ToByte(g + (1.0f - g) * k),
                 ToByte(b + (1.0f - b) * k), 255);
}

// D2.8：colormap 采样（t ∈ [-1,1] → RGB），预设 8 档
inline QRgb SampleColorMap(SectionColorMapType type, float t) {
    switch (type) {
    case SectionColorMapType::RedWhiteBlue: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return qRgba(ToByte(0.85f * k), ToByte(0.90f * k), 255, 255);
        return qRgba(255, ToByte(0.88f * k), ToByte(0.84f * k), 255);
    }
    case SectionColorMapType::BlueWhiteRed: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return qRgba(255, ToByte(0.88f * k), ToByte(0.84f * k), 255);
        return qRgba(ToByte(0.85f * k), ToByte(0.90f * k), 255, 255);
    }
    case SectionColorMapType::Grayscale: {
        const unsigned char g = ToByte((t + 1.0f) * 0.5f);
        return qRgba(g, g, g, 255);
    }
    case SectionColorMapType::BlackWhiteBlue: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return qRgba(ToByte(0.85f * k), ToByte(0.90f * k), 255, 255);
        const unsigned char g = ToByte(k);
        return qRgba(g, g, g, 255); // Peak→Black
    }
    case SectionColorMapType::RedWhiteBlack: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return qRgba(ToByte(k), ToByte(k), ToByte(k), 255); // Trough→Black
        return qRgba(255, ToByte(0.88f * k), ToByte(0.84f * k), 255);
    }
    case SectionColorMapType::GreenWhiteMagenta: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return WhiteAnchoredRamp(k, 1.0f, 0.0f, 1.0f);   // Trough→Magenta
        return WhiteAnchoredRamp(k, 0.0f, 0.78f, 0.20f);     // Peak→Green
    }
    case SectionColorMapType::CyanWhiteOrange: {
        const float mag = std::pow(std::abs(t), 0.85f);
        const float k = 1.0f - mag;
        if (t < 0.0f)
            return WhiteAnchoredRamp(k, 1.0f, 0.55f, 0.10f); // Trough→Orange
        return WhiteAnchoredRamp(k, 0.0f, 0.75f, 1.0f);      // Peak→Cyan
    }
    case SectionColorMapType::Rainbow:
    default: {
        const float u = std::clamp((t + 1.0f) * 0.5f, 0.0f, 1.0f);
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (u < 0.25f) {
            const float f = u / 0.25f;
            b = 1.0f; g = f;
        } else if (u < 0.5f) {
            const float f = (u - 0.25f) / 0.25f;
            g = 1.0f; b = 1.0f - f;
        } else if (u < 0.75f) {
            const float f = (u - 0.5f) / 0.25f;
            g = 1.0f; r = f;
        } else {
            const float f = (u - 0.75f) / 0.25f;
            r = 1.0f; g = 1.0f - f;
        }
        return qRgba(ToByte(r), ToByte(g), ToByte(b), 255);
    }
    }
}

} // namespace seismic
