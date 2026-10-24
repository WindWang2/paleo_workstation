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
// Prediction codes are microfacies. Automatic display of prediction products
// keeps their native level instead of aggregating them as map scale changes.
QString productDisplayMode(const QString &kind, const QString &mode = QStringLiteral("auto"));
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
// Class-level display pins: node key (JSON path of length 1..3, as produced by
// key()) -> fixed level. A pin covers the node's whole subtree; the deepest
// (nearest) pinned ancestor of a leaf wins. Invalid entries are dropped.
QVariantMap parsePins(const QString &json);
QString encodePins(const QVariantMap &pins);
// Pinned level for one classification row, or empty when no pin applies.
QString pinnedLevel(const QVariantMap &pins, const QVariantMap &f);
// Unique pinnable nodes of a schema: {key, level, path} ordered by path.
QVariantList pinCandidates(const QVariantList &schema);
// Change a parent while retaining compatible descendants in the target tree.
int reassignedCode(const QVariantList &schema, int oldCode, int targetCode,
                   const QString &level);
} // namespace FaciesHierarchy
