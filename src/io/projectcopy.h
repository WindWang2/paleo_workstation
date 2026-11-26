// 层：数据
#pragma once
#include <QStringList>
#include <functional>

struct PaleoProjectFile;
namespace paleo::projectcopy {
bool relocateCopiedLayers(const PaleoProjectFile &file, const QString &origin, const QString &stage,
                          const QString &target, QString &error);
bool inspectStoredBindings(const QString &copy, const QString &original, const QString &stage, QStringList &review, QString &error);
// Copy a quiescent project folder into a new child of parent and preserve its relative paths.
// Return the new entry file only after a same-volume rename of the verified private copy.
QString copyVerified(const QString &entry, const QString &parent, const QString &requiredRoot, QString &error,
                     const std::function<bool()> &cancelled = {},
                     const std::function<void(qint64, qint64)> &progress = {}, QStringList *sourceReview = nullptr);
}
