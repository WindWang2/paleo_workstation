// 层：视图
#pragma once

#include <QRgb>
#include <QString>
#include <QStringList>
#include <QVector>
#include <vector>

#include "domain/seismic/sgyvolume.h"

namespace seismic {

// D3.5 三维 colormap：预设 + 自定义控制点 → 256 档 LUT → 切片重着色
// （CPU 侧从 values 重算 rgba 再上传纹理——3D 不再被 vendor 预烘焙色锁死）。
struct Seismic3DColorStop
{
    float pos = 0.0f;   // 0..1（0=负峰，1=正峰）
    QRgb color = 0;     // 预乘无关的 ARGB
};

class Seismic3DColorMap
{
public:
    // 预设名列表（与 2D 剖面色标语义对齐 + 三维扩展）
    static QStringList presetNames();
    static Seismic3DColorMap preset(const QString &name);

    QString name() const { return name_; }
    void setName(const QString &name) { name_ = name; }

    const QVector<Seismic3DColorStop> &stops() const { return stops_; }
    void setStops(const QVector<Seismic3DColorStop> &stops); // 自动按 pos 排序

    bool inverted() const { return inverted_; }
    void setInverted(bool inverted) { inverted_ = inverted; }

    // 256 档 LUT（线性插值控制点；inverted 交换首尾）
    std::vector<QRgb> buildLut() const;

    // 用 LUT 重算切片 rgba（NaN → #303131 深灰，同 2D 语义）
    void colorizeSlice(SgySliceImage &image, float gain, float contrast) const;

private:
    QString name_;
    QVector<Seismic3DColorStop> stops_;
    bool inverted_ = false;
};

} // namespace seismic
