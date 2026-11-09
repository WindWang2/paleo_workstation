// 层：数据
#pragma once
#include <QStringList>
#include <QVariantMap>
class DataCatalog;
namespace paleo {
QVariantMap timeDepthAlignmentOptions(DataCatalog *catalog,const QStringList &wellIds,QString *error);
// A successful SDK write may return a warning if the Catalog projection needs reopen recovery.
bool saveTimeDepthAlignment(DataCatalog *catalog,const QVariantMap &selection,QString *error);
}
