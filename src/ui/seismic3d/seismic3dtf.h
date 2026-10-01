// 层：视图
#pragma once

#include <QRgb>
#include <QString>
#include <QStringList>
#include <QVector>

#include <vector>

#include "domain/seismic/sgyvolume.h"

namespace seismic {

// 体渲染传递函数停靠点：pos 0..1（0=负峰 1=正峰，与 Seismic3DColorMap 同
// 归一语义——absMax 对称归一后 (v/absMax+1)/2），color=RGB，alpha=不透明度。
struct Seismic3DTfStop
{
    float pos = 0.0f;
    QRgb color = 0;
    float alpha = 1.0f;
};

// D7.1 体渲染传递函数（线性分段）：色彩+不透明度统一停靠点 → 256 档 RGBA
// LUT。GPU 路线：LUT 上传 GL_TEXTURE_1D，切片/堆叠层纹理改传归一化索引
// （GL_RG8：R=LUT 索引、G=有效掩码），TF 修改只重传 256B LUT——零取数零
// 重烘焙（区别于 D3.5 colormap 的 CPU 重着色路径）。
class Seismic3DTransferFunction
{
public:
    static QStringList presetNames();
    static Seismic3DTransferFunction preset(const QString &name);

    void setStops(const QVector<Seismic3DTfStop> &stops); // 自动按 pos 排序
    const QVector<Seismic3DTfStop> &stops() const { return stops_; }

    void setStopAlpha(int index, float alpha); // 编辑器拖纵（0..1）
    void setStopPos(int index, float pos);     // 编辑器拖横（邻居区间内）
    void setStopColor(int index, QRgb color);
    int addStop(float pos, QRgb color, float alpha); // 插入排序位，返回序号
    bool removeStop(int index);                      // 始终保留 ≥2 停靠点

    [[nodiscard]] bool isValid() const { return stops_.size() >= 2; }

    // 256×4 straight-alpha RGBA（GL_TEXTURE_1D 上传原料）
    [[nodiscard]] std::vector<unsigned char> buildLutRgba() const;

    // 值→LUT 归一化索引（与切片值纹理编码逐位一致；gain/contrast 恒 1——
    // TF 是完整替换管线，编辑器所见即渲染所得）
    static unsigned char valueToIndex(float v, float absMax);
    // 切片 → 值纹理字节（w*h*2：R=索引 G=有效掩码），NaN → G=0
    static std::vector<unsigned char> buildIndexBytes(const SgySliceImage &image);

private:
    QVector<Seismic3DTfStop> stops_;
};

} // namespace seismic
