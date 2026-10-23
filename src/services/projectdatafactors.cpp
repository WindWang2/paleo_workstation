// 层：数据
#include "projectdata.h"
#include "../catalog/datacatalog.h"
#include <QMap>
QVariantList ProjectDataFacade::importedWellFactors(const QString &horizon, QString *error) const {
  if (error) error->clear();
  if (!m_catalog) return {};
  QMap<QString, QVariantMap> rows;
  for (const auto &asset : m_catalog->assets()) {
    const auto version = m_catalog->currentVersion(asset.id);
    for (const auto &value : version.extra.value("wellFactors").toList()) {
      auto row = value.toMap();
      if (row.value("horizon").toString().compare(horizon, Qt::CaseInsensitive) != 0) continue;
      const auto key = DataCatalog::normalizeWellName(row.value("well_name").toString());
      const auto matches = m_catalog->wellsMatchingName(key);
      if (matches.size() != 1) {
        if (error)
          *error = matches.isEmpty()
                       ? tr("井名 %1 对不上任何井，不能自动关联单因素文件").arg(row.value("well_name").toString())
                       : tr("井名 %1 对应多个井实体，不能自动关联单因素文件").arg(key);
        return {};
      }
      if (rows.contains(key)) {
        auto previous = rows.value(key);
        for (const auto *field : {"_source_version", "_source_path"}) previous.remove(QLatin1String(field));
        if (previous != row) {
          if (error) *error = tr("井 %1 的 %2 单因素文件存在冲突，请只保留一份有效输入").arg(key, horizon);
          return {};
        }
        continue;
      }
      row.insert("_source_version", version.id);
      row.insert("_source_path", DataCatalog::resolvedVersionPath(projectDir(), version));
      rows.insert(key, row);
    }
  }
  QVariantList result;
  for (const auto &row : rows) result << row;
  return result;
}
