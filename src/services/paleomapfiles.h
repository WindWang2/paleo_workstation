// 层：数据
#pragma once
#include <QStringList>
#include <QVariantMap>
#include <functional>
namespace PaleoMapFiles {
QStringList discover(const QStringList &files, const QString &folder,
                     const std::function<bool()> &cancelled = {});
QStringList members(const QString &path, QString *error);
QString inferredHorizon(const QString &path, const QStringList &horizons, const QString &fallback);
// Copy and hash every member; changes to DBF/PRJ are part of dataset identity.
QVariantList copyDataset(const QString &source, const QString &directory,
                        const std::function<bool()> &cancelled, QString *error);
QString fingerprint(const QVariantList &members);
}
