// 层：数据
#pragma once
#include <QStringList>
#include <QVariantList>
namespace FaciesHierarchy {
struct ScaleThresholds {
  double faciesToSubfacies = 250000;
  double subfaciesToMicrofacies = 50000;
};
bool validThresholds(const ScaleThresholds &thresholds);
QStringList levels();
QString title(const QString &level);
QString resolveLevel(const QString &mode, double scale,
                     const ScaleThresholds &thresholds = {});
// Legacy missing levels are explicitly marked as undefined; never copy a parent
// name into a child. Full paths distinguish homonymous children.
QStringList path(const QVariantMap &f);
QString key(const QVariantMap &f, const QString &level);
QVariantList legend(const QVariantList &schema, const QString &level,
                    const QVariantList &codes);
// Expand a level legend into ordered parent headings and leaf rows for display.
QVariantList legendTree(const QVariantList &entries);
// Change a parent while retaining compatible descendants in the target tree.
int reassignedCode(const QVariantList &schema, int oldCode, int targetCode,
                   const QString &level);
} // namespace FaciesHierarchy
