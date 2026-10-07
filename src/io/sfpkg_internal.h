// 层：数据
#pragma once

// 方向70（Unity 清障）：sfpkgreader/sfpkgwriter 匿名 namespace 各带一份同构
// helper（sha256Hex 哈希包装 + sfpkg 归档 entry 名常量），UNITY_BUILD 合批
// 即重定义。收拢单一定义；两 .cpp 经 `using paleo::io_detail::…;` 零改动。
//（shacache.h 的同名成员函数是 Shacache 类方法，不同实体不受影响。）

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

namespace paleo::io_detail {

/// SHA-256 十六进制小写摘要。
inline QString sha256Hex(const QByteArray &bytes)
{
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

/// sfpkg 归档 entry 契约（读写两侧共用，改这里 = 改格式版本）。
inline const QString kManifestEntry = QStringLiteral("manifest.json");
inline const QString kNpzEntry = QStringLiteral("surface.npz");
inline const QString kChecksumEntry = QStringLiteral("checksum.json");

} // namespace paleo::io_detail
