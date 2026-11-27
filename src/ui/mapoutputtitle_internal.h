// 层：视图
#pragma once

#include "../catalog/datacatalog.h"

namespace paleo::ui_detail {
inline QString mapOutputTitle(const CatalogVersion &version)
{
  if (version.stage != QLatin1String("OUTPUT")) return {};
  const auto history = version.extra.value("map_exports").toList();
  const QString horizon = version.extra.value("horizon").toString();
  for (auto it = history.crbegin(); it != history.crend(); ++it) {
    const auto record = it->toMap();
    const auto context = record.value("context").toMap();
    if (!horizon.isEmpty() && context.value("horizon").toString() != horizon) continue;
    const QString title = record.value("title").toString().trimmed();
    if (!title.isEmpty()) return title;
    // Historical exports did not record the layout title. A layer may be
    // repeated in the inset, but it must identify the same single source.
    const auto inputs = record.value("inputs").toStringList();
    if (inputs.size() != 1 || inputs.first().isEmpty()) return {};
    QString sourceTitle;
    for (const auto &item : context.value("layers").toList()) {
      const auto layer = item.toMap();
      const QString name = layer.value("title").toString().trimmed();
      if (layer.value("version_id").toString() != inputs.first() || name.isEmpty() ||
          (!sourceTitle.isEmpty() && sourceTitle != name)) return {};
      sourceTitle = name;
    }
    return sourceTitle;
  }
  return {};
}
}
