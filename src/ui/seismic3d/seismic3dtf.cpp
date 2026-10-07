// 层：视图
// token 例外：DESIGN 数据符号例外：体渲染颜色/透明度传递函数预设，属于可导出的渲染数据。（tools/ui-token-exceptions.json 精确计数）。
#include "seismic3dtf.h"
#include "seismic3d_internal.h"

#include <QColor>

#include <algorithm>
#include <cmath>

namespace seismic {

namespace {
} // namespace

QStringList Seismic3DTransferFunction::presetNames()
{
    return {QStringLiteral("均匀半透明"),
            QStringLiteral("双峰高亮"),
            QStringLiteral("正峰体"),
            QStringLiteral("递进不透明")};
}

Seismic3DTransferFunction Seismic3DTransferFunction::preset(const QString &name)
{
    Seismic3DTransferFunction tf;
    const auto stop = [](float pos, const QColor &c, float alpha) {
        return Seismic3DTfStop{pos, c.rgba(), alpha};
    };
    if (name == QStringLiteral("均匀半透明"))
    {
        // 软体透视：全值域低不透明度，适合整体构造浏览
        tf.setStops({stop(0.0f, QColor(25, 118, 210), 0.28f),
                     stop(0.5f, QColor(255, 255, 255), 0.16f),
                     stop(1.0f, QColor(220, 38, 38), 0.28f)});
    }
    else if (name == QStringLiteral("双峰高亮"))
    {
        // 挖空中段、只留强负/强正反射（河道/礁体侦察）
        tf.setStops({stop(0.0f, QColor(25, 118, 210), 0.85f),
                     stop(0.34f, QColor(120, 160, 210), 0.0f),
                     stop(0.66f, QColor(226, 150, 150), 0.0f),
                     stop(1.0f, QColor(220, 38, 38), 0.85f)});
    }
    else if (name == QStringLiteral("正峰体"))
    {
        // 压制负反射、亮出正反射（亮点/流体检波）
        tf.setStops({stop(0.0f, QColor(25, 118, 210), 0.0f),
                     stop(0.52f, QColor(255, 255, 255), 0.04f),
                     stop(1.0f, QColor(220, 38, 38), 0.92f)});
    }
    else // 递进不透明
    {
        tf.setStops({stop(0.0f, QColor(20, 20, 24), 0.0f),
                     stop(0.5f, QColor(140, 140, 150), 0.3f),
                     stop(1.0f, QColor(255, 255, 255), 1.0f)});
    }
    return tf;
}

void Seismic3DTransferFunction::setStops(const QVector<Seismic3DTfStop> &stops)
{
    stops_ = stops;
    std::sort(stops_.begin(), stops_.end(),
              [](const Seismic3DTfStop &a, const Seismic3DTfStop &b) { return a.pos < b.pos; });
}

void Seismic3DTransferFunction::setStopAlpha(int index, float alpha)
{
    if (index < 0 || index >= stops_.size())
        return;
    stops_[index].alpha = std::clamp(alpha, 0.0f, 1.0f);
}

void Seismic3DTransferFunction::setStopPos(int index, float pos)
{
    if (index < 0 || index >= stops_.size())
        return;
    const float lo = index > 0 ? stops_[index - 1].pos + 0.01f : 0.0f;
    const float hi = index + 1 < stops_.size() ? stops_[index + 1].pos - 0.01f : 1.0f;
    stops_[index].pos = std::clamp(pos, lo, std::max(lo, hi));
}

void Seismic3DTransferFunction::setStopColor(int index, QRgb color)
{
    if (index < 0 || index >= stops_.size())
        return;
    stops_[index].color = color;
}

int Seismic3DTransferFunction::addStop(float pos, QRgb color, float alpha)
{
    Seismic3DTfStop stop;
    stop.pos = std::clamp(pos, 0.0f, 1.0f);
    stop.color = color;
    stop.alpha = std::clamp(alpha, 0.0f, 1.0f);
    int insertAt = 0;
    while (insertAt < stops_.size() && stops_[insertAt].pos <= stop.pos)
        ++insertAt;
    stops_.insert(insertAt, stop);
    return insertAt;
}

bool Seismic3DTransferFunction::removeStop(int index)
{
    if (stops_.size() <= 2 || index < 0 || index >= stops_.size())
        return false;
    stops_.removeAt(index);
    return true;
}

unsigned char Seismic3DTransferFunction::valueToIndex(float v, float absMax)
{
    const float scale = absMax > 1e-8f ? 1.0f / absMax : 1.0f;
    const float t = (std::clamp(v * scale, -1.0f, 1.0f) + 1.0f) * 0.5f;
    return ToByte(t);
}

std::vector<unsigned char> Seismic3DTransferFunction::buildIndexBytes(const SgySliceImage &image)
{
    if (image.values.empty())
        return {};
    const float absMax = std::max(std::abs(image.valueMin), std::abs(image.valueMax));
    std::vector<unsigned char> bytes(image.values.size() * 2);
    for (std::size_t i = 0; i < image.values.size(); ++i)
    {
        const float v = image.values[i];
        if (std::isfinite(v))
        {
            bytes[i * 2 + 0] = valueToIndex(v, absMax);
            bytes[i * 2 + 1] = 255;
        }
        else
        {
            bytes[i * 2 + 0] = 0;
            bytes[i * 2 + 1] = 0; // 缺失掩码（shader discard）
        }
    }
    return bytes;
}

std::vector<unsigned char> Seismic3DTransferFunction::buildLutRgba() const
{
    std::vector<unsigned char> lut(256 * 4);
    if (stops_.isEmpty())
    {
        for (int i = 0; i < 256; ++i)
        {
            lut[std::size_t(i) * 4 + 0] = 128;
            lut[std::size_t(i) * 4 + 1] = 128;
            lut[std::size_t(i) * 4 + 2] = 128;
            lut[std::size_t(i) * 4 + 3] = 255;
        }
        return lut;
    }
    const int n = static_cast<int>(stops_.size());
    for (int i = 0; i < 256; ++i)
    {
        const float pos = i / 255.0f;
        int k = 0;
        while (k < n - 2 && stops_[k + 1].pos < pos)
            ++k;
        const float span = std::max(1e-6f, stops_[k + 1].pos - stops_[k].pos);
        const float t = std::clamp((pos - stops_[k].pos) / span, 0.0f, 1.0f);
        const QColor a(stops_[k].color);
        const QColor b(stops_[k + 1].color);
        const float lerpAlpha =
            stops_[k].alpha + t * (stops_[k + 1].alpha - stops_[k].alpha);
        lut[std::size_t(i) * 4 + 0] = ToByte(a.redF() + t * (b.redF() - a.redF()));
        lut[std::size_t(i) * 4 + 1] = ToByte(a.greenF() + t * (b.greenF() - a.greenF()));
        lut[std::size_t(i) * 4 + 2] = ToByte(a.blueF() + t * (b.blueF() - a.blueF()));
        lut[std::size_t(i) * 4 + 3] = ToByte(lerpAlpha);
    }
    return lut;
}

} // namespace seismic
