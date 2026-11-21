// 层：功能
#pragma once
#include "../catalog/datacatalog.h"
#include <QString>
#include <QVariantMap>
#include <functional>

// 图件空间编辑历史：新版本只存本批 delta，旧累计列表原样保留。
// 读取沿 spatial_edit_history.parent_version_id，缺省时仅当计算父版本
// 恰好一个才跟随。证据 REFERENCES、tombstone 与 interpretation_reference_versions
// 不是编辑链。
namespace paleo::mapedit {
inline constexpr int kSpatialHistoryPageMax = 200;
inline constexpr char kSpatialHistorySchema[] = "paleo_spatial_edit_delta_v1";

struct SpatialEditHistoryQuery {
  QString versionId;
  QString featureUid;
  int pageSize = 50;
  QString cursor;
};

QVariantMap spatialEditHistoryLink(const CatalogVersion &previous);
QString spatialEventKey(const QVariantMap &event, QString *diagnostic);
QVariantMap readSpatialEditHistory(const std::function<CatalogVersion(const QString &)> &lookup,
                                   const SpatialEditHistoryQuery &query);
}
