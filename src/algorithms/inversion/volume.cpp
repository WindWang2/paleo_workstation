// 层：数据
#include "volume.h"

#include <QJsonDocument>

#include <cstddef>
#include <cstring>

namespace paleo::inversion
{
namespace
{

constexpr char kMagic[] = "IIMP1\n";

} // namespace

QByteArray writeImpedanceVolumeBlob(const std::vector<float> &volume, const QJsonObject &header)
{
  const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
  QByteArray bytes;
  bytes.reserve(int(sizeof(kMagic) - 1) + 4 + json.size() + int(volume.size() * sizeof(float)));
  bytes.append(kMagic, int(sizeof(kMagic) - 1));
  const quint32 jsonLen = quint32(json.size());
  unsigned char lenBytes[4] = {static_cast<unsigned char>(jsonLen & 0xFF),
                              static_cast<unsigned char>((jsonLen >> 8) & 0xFF),
                              static_cast<unsigned char>((jsonLen >> 16) & 0xFF),
                              static_cast<unsigned char>((jsonLen >> 24) & 0xFF)};
  bytes.append(reinterpret_cast<const char *>(lenBytes), 4);
  bytes.append(json);
  bytes.append(reinterpret_cast<const char *>(volume.data()),
               int(volume.size() * sizeof(float)));
  return bytes;
}

bool readImpedanceVolumeBlob(const QByteArray &blob, std::vector<float> *volume,
                             QJsonObject *header, QString *error)
{
  if (error)
    error->clear();
  const int magicLen = int(sizeof(kMagic) - 1);
  if (blob.size() < magicLen + 4 || std::memcmp(blob.constData(), kMagic, size_t(magicLen)) != 0)
  {
    if (error)
      *error = QStringLiteral("IIMP1 magic 不符（非阻抗体文件）");
    return false;
  }
  const unsigned char *lenBytes =
      reinterpret_cast<const unsigned char *>(blob.constData() + magicLen);
  const quint32 jsonLen = quint32(lenBytes[0]) | (quint32(lenBytes[1]) << 8) |
                          (quint32(lenBytes[2]) << 16) | (quint32(lenBytes[3]) << 24);
  const int bodyOffset = magicLen + 4 + int(jsonLen);
  if (bodyOffset > blob.size())
  {
    if (error)
      *error = QStringLiteral("IIMP1 头长度越界");
    return false;
  }
  QJsonParseError parse{};
  const QJsonDocument doc =
      QJsonDocument::fromJson(QByteArray(blob.constData() + magicLen + 4, int(jsonLen)), &parse);
  if (parse.error != QJsonParseError::NoError || !doc.isObject())
  {
    if (error)
      *error = QStringLiteral("IIMP1 头 JSON 解析失败: ") + parse.errorString();
    return false;
  }
  const QJsonObject root = doc.object();
  const qint64 nIl = root.value(QStringLiteral("nIl")).toInt(-1);
  const qint64 nXl = root.value(QStringLiteral("nXl")).toInt(-1);
  const qint64 nS = root.value(QStringLiteral("nS")).toInt(-1);
  if (nIl > 0 && nXl > 0 && nS > 0)
  {
    const qint64 expect = nIl * nXl * nS * qint64(sizeof(float));
    if (blob.size() - bodyOffset != expect)
    {
      if (error)
        *error = QStringLiteral("IIMP1 体字节数 %1 ≠ 头声明 %2")
                     .arg(qint64(blob.size()) - bodyOffset)
                     .arg(expect);
      return false;
    }
  }
  if (volume)
  {
    const int count = (blob.size() - bodyOffset) / int(sizeof(float));
    volume->resize(std::size_t(count > 0 ? count : 0));
    if (count > 0)
      std::memcpy(volume->data(), blob.constData() + bodyOffset, size_t(count) * sizeof(float));
  }
  if (header)
    *header = root;
  return true;
}

} // namespace paleo::inversion
