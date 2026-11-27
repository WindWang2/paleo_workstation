// 层：数据
#pragma once
#include <QRegularExpression>
#include <QString>
#include "stratigraphicunit.h"

inline QString workspaceVersionCaption(const QString &title, int version)
{
  if (version <= 0) return title;
  static const QRegularExpression token(QStringLiteral("(?:^|[^A-Za-z0-9])v([0-9]+)(?![A-Za-z0-9])"),
                                        QRegularExpression::CaseInsensitiveOption);
  auto versions = token.globalMatch(title);
  while (versions.hasNext())
    if (versions.next().captured(1).toInt() == version) return title;
  return QStringLiteral("%1 · v%2").arg(title).arg(version);
}

namespace paleo::mapcaption {
inline QString productTitle(QString title, const QString &horizon, const QVariantMap &savedUnit,
                            const strat::UnitSelection &selected, int version, bool tree = false)
{
  auto unit = strat::fromMap(savedUnit);
  if (unit.displayName.isEmpty() && horizon == strat::analysisLayerKey(selected)) unit = selected;
  const auto key = strat::analysisLayerKey(unit);
  if (key.isEmpty() || unit.displayName.isEmpty()) return workspaceVersionCaption(title, version);
  const QString separator = QStringLiteral(" · ");
  if (tree && title.startsWith(key)) {
    title = title.mid(key.size()).trimmed();
    if (title.startsWith(QChar(0x00b7))) title = title.mid(1).trimmed();
    const QString versionTag = QStringLiteral("v%1").arg(version);
    if (version > 0 && title.endsWith(separator + versionTag, Qt::CaseInsensitive))
      title.chop(separator.size() + versionTag.size());
    if (version > 0 && workspaceVersionCaption(title, version) != title)
      title.prepend(versionTag + separator);
    return title + separator + unit.displayName;
  }
  if (title.contains(key)) title.replace(key, unit.displayName);
  else if (key == horizon && !title.contains(unit.displayName)) title.prepend(unit.displayName + separator);
  return workspaceVersionCaption(title, version);
}
}
