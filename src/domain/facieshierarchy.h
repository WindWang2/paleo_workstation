// 层：数据
#pragma once
#include <QStringList>
#include <QVariantList>
namespace FaciesHierarchy {
QStringList levels();
QString title(const QString &level);
QString resolveLevel(const QString &mode, double scale);
// Missing fields inherit the closest ancestor, then the closest descendant.
// Stored source values remain untouched; paths distinguish homonymous children.
QStringList path(const QVariantMap &f);
QString key(const QVariantMap &f, const QString &level);
QVariantList legend(const QVariantList &schema, const QString &level,
                    const QVariantList &codes);
// Change a parent while retaining compatible descendants in the target tree.
int reassignedCode(const QVariantList &schema, int oldCode, int targetCode,
                   const QString &level);
} // namespace FaciesHierarchy
