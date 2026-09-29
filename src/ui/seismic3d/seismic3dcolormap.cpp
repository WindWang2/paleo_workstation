// 层：视图
#include "seismic3dcolormap.h"

#include <QColor>
#include <algorithm>
#include <cmath>

namespace seismic {

namespace {
inline unsigned char ToByte(float v)
{
    const int val = static_cast<int>(std::round(v * 255.0f));
    return static_cast<unsigned char>(std::clamp(val, 0, 255));
}
} // namespace

QStringList Seismic3DColorMap::presetNames()
{
    return {QStringLiteral("红白蓝"),
            QStringLiteral("蓝白红"),
            QStringLiteral("灰度"),
            QStringLiteral("彩虹谱"),
            QStringLiteral("黑-白-蓝"),
            QStringLiteral("绿-白-品红"),
            QStringLiteral("青-白-橙"),
            QStringLiteral("发热体")};
}

Seismic3DColorMap Seismic3DColorMap::preset(const QString &name)
{
    Seismic3DColorMap cmap;
    cmap.setName(name);
    const auto stop = [](float pos, const QColor &c) {
        return Seismic3DColorStop{pos, c.rgba()};
    };
    if (name == QStringLiteral("红白蓝"))
    {
        cmap.setStops({stop(0.0f, QColor(25, 118, 210)), stop(0.5f, QColor(255, 255, 255)),
                       stop(1.0f, QColor(220, 38, 38))});
    }
    else if (name == QStringLiteral("蓝白红"))
    {
        cmap.setStops({stop(0.0f, QColor(220, 38, 38)), stop(0.5f, QColor(255, 255, 255)),
                       stop(1.0f, QColor(25, 118, 210))});
    }
    else if (name == QStringLiteral("灰度"))
    {
        cmap.setStops({stop(0.0f, QColor(255, 255, 255)), stop(1.0f, QColor(0, 0, 0))});
    }
    else if (name == QStringLiteral("彩虹谱"))
    {
        cmap.setStops({stop(0.0f, QColor(21, 101, 192)), stop(0.25f, QColor(0, 172, 193)),
                       stop(0.5f, QColor(56, 142, 60)), stop(0.75f, QColor(251, 192, 45)),
                       stop(1.0f, QColor(220, 38, 38))});
    }
    else if (name == QStringLiteral("黑-白-蓝"))
    {
        cmap.setStops({stop(0.0f, QColor(255, 255, 255)), stop(0.5f, QColor(255, 255, 255)),
                       stop(1.0f, QColor(20, 20, 24))});
    }
    else if (name == QStringLiteral("绿-白-品红"))
    {
        cmap.setStops({stop(0.0f, QColor(216, 27, 216)), stop(0.5f, QColor(255, 255, 255)),
                       stop(1.0f, QColor(46, 160, 67))});
    }
    else if (name == QStringLiteral("青-白-橙"))
    {
        cmap.setStops({stop(0.0f, QColor(255, 138, 25)), stop(0.5f, QColor(255, 255, 255)),
                       stop(1.0f, QColor(0, 172, 193))});
    }
    else // 发热体（黑-红-黄-白）
    {
        cmap.setStops({stop(0.0f, QColor(10, 10, 30)), stop(0.35f, QColor(183, 28, 28)),
                       stop(0.7f, QColor(251, 192, 45)), stop(1.0f, QColor(255, 255, 255))});
    }
    return cmap;
}

void Seismic3DColorMap::setStops(const QVector<Seismic3DColorStop> &stops)
{
    stops_ = stops;
    std::sort(stops_.begin(), stops_.end(),
              [](const Seismic3DColorStop &a, const Seismic3DColorStop &b) { return a.pos < b.pos; });
}

std::vector<QRgb> Seismic3DColorMap::buildLut() const
{
    std::vector<QRgb> lut(256);
    if (stops_.isEmpty())
    {
        for (auto &c : lut)
            c = qRgba(128, 128, 128, 255);
        return lut;
    }
    const int n = static_cast<int>(stops_.size());
    for (int i = 0; i < 256; ++i)
    {
        float pos = i / 255.0f;
        if (inverted_)
            pos = 1.0f - pos;
        // 找到插值区间
        int k = 0;
        while (k < n - 2 && stops_[k + 1].pos < pos)
            ++k;
        const float span = std::max(1e-6f, stops_[k + 1].pos - stops_[k].pos);
        const float t = std::clamp((pos - stops_[k].pos) / span, 0.0f, 1.0f);
        const QColor a(stops_[k].color);
        const QColor b(stops_[k + 1].color);
        lut[static_cast<std::size_t>(i)] = qRgba(
            ToByte(a.red() / 255.0f + t * (b.red() - a.red()) / 255.0f),
            ToByte(a.green() / 255.0f + t * (b.green() - a.green()) / 255.0f),
            ToByte(a.blue() / 255.0f + t * (b.blue() - a.blue()) / 255.0f), 255);
    }
    return lut;
}

void Seismic3DColorMap::colorizeSlice(SgySliceImage &image, float gain, float contrast) const
{
    if (image.values.empty())
        return;
    const std::vector<QRgb> lut = buildLut();
    const float absMax = std::max(std::abs(image.valueMin), std::abs(image.valueMax));
    const float baseScale = absMax > 1e-8f ? 1.0f / absMax : 1.0f;

    if (image.rgba.size() < image.values.size() * 4)
        image.rgba.resize(image.values.size() * 4);
    for (std::size_t i = 0; i < image.values.size(); ++i)
    {
        const float raw = image.values[i];
        const QRgb c = !std::isfinite(raw)
            ? qRgba(48, 49, 49, 255) // NaN=缺失（DESIGN.md #303131）
            : lut[static_cast<std::size_t>(std::clamp(int((std::clamp(raw * baseScale * gain * contrast, -1.0f, 1.0f) + 1.0f) * 127.5f), 0, 255))];
        image.rgba[i * 4 + 0] = static_cast<unsigned char>(qRed(c));
        image.rgba[i * 4 + 1] = static_cast<unsigned char>(qGreen(c));
        image.rgba[i * 4 + 2] = static_cast<unsigned char>(qBlue(c));
        image.rgba[i * 4 + 3] = static_cast<unsigned char>(qAlpha(c));
    }
}

} // namespace seismic
