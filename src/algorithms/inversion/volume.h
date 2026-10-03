// 层：数据
#pragma once

// inversion/volume — 阻抗体自描述 blob（IIMP1）。
//
// 格式：magic "IIMP1\n" + u32(头 JSON 字节数) + JSON 头 + float32 体（小端）。
// 体布局 ((il·nXl)+xl)·nS+s，NaN = 缺失道/缺失样。无时间戳——同输入字节级
// 稳定（PPROP1 同模式，断面对拍/缓存可复现）。
// 头部 JSON 必带 nIl/nXl/nS（读写双方校验体字节数一致），其余键（t0Ms/dtMs/
// 测网域/几何/方法参数/频段口径）由写方负责完整、读方原样透传。

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <vector>

namespace paleo::inversion
{

QByteArray writeImpedanceVolumeBlob(const std::vector<float> &volume, const QJsonObject &header);

bool readImpedanceVolumeBlob(const QByteArray &blob, std::vector<float> *volume,
                             QJsonObject *header, QString *error = nullptr);

} // namespace paleo::inversion
