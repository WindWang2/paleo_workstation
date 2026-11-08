// 层：数据
#pragma once

#include <QSet>
#include <QVariantMap>

class DataCatalog;
namespace paleo::document {
struct ModelObjectRef;
struct RelationResolveRequest;
struct RelationUseSpec;

// Add only after a numerical consumer successfully uses the captured table.
QVariantMap timeDepthDependency(const RelationResolveRequest &request);
bool validateTimeDepthDependencies(const DataCatalog &catalog, const QVariantList &receipts, QString *error = nullptr);

// Call inside the same Catalog batch as the primary choice.
bool invalidateTimeDepthSelection(DataCatalog &catalog, const RelationUseSpec &use,
                                  const ModelObjectRef *selected, QString *error = nullptr);
// Recover the Catalog projection after an SDK write, or at writable session open.
bool refreshTimeDepthDependencies(DataCatalog &catalog, QString *error = nullptr);
bool refreshTimeDepthObjectDependencies(DataCatalog &catalog, qlonglong tableId, QString *error = nullptr);
bool refreshTimeDepthDependencies(DataCatalog &catalog, const QSet<qlonglong> &sources,
                                  qlonglong volume, QString *error = nullptr);
}
