// 层：数据
#pragma once
#include "pythondelivery.h"
#include <QByteArray>
#include <QJsonObject>
#include <QVector>

namespace paleo::python::delivery {
inline constexpr qint64 MaxTree = 1024LL * 1024 * 1024 + 4 * 1024 * 1024;
inline constexpr qint64 MaxHeader = 4 * 1024 * 1024;
struct Entry {
  QString path, sha256;
  qint64 size = 0;
  bool executable = false;
};
struct Header {
  QString kind, platform, manifestSha256, licenseStatus;
  QVector<Entry> entries;
  qint64 treeSize = 0;
};
bool digest(const QString &value);
bool parseHeader(const QByteArray &raw, Header &header);
bool storeEntries(const Header &header, const QString &store);
QString validateManifest(const Header &header, const QByteArray &raw);
QString verifyInstalled(const Header &header, const QString &root,
                        const std::function<bool()> &cancelled);
} // namespace paleo::python::delivery
