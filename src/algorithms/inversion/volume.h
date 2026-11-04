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
#include <memory>
class QSaveFile;

namespace paleo::inversion
{

QByteArray writeImpedanceVolumeBlob(const std::vector<float> &volume, const QJsonObject &header);

bool readImpedanceVolumeBlob(const QByteArray &blob, std::vector<float> *volume,
                             QJsonObject *header, QString *error = nullptr);
bool readImpedanceVolumeHeader(const QString& path,QJsonObject* header,QString* error=nullptr);

// Bounded writer for native bodies. Reserved JSON space allows final QC to be
// patched without holding or rewriting the volume. Finish atomically commits.
class ImpedanceVolumeStream {
public:
 explicit ImpedanceVolumeStream(const QString& path);
 ~ImpedanceVolumeStream();
 bool open(const QJsonObject& header,QString* error);
 bool append(const float* values,qint64 count,QString* error);
 bool finish(const QJsonObject& header,QString* error);
private:
 std::unique_ptr<QSaveFile> m_file;
 qint64 m_expected=0,m_written=0;
};

} // namespace paleo::inversion
