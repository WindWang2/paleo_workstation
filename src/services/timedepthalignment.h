// 层：数据
#pragma once
#include <QStringList>
#include <QVariantMap>
class DataCatalog;
namespace paleo {
QVariantMap timeDepthAlignmentOptions(DataCatalog *catalog,const QStringList &wellIds,QString *error);
bool saveTimeDepthAlignment(DataCatalog *catalog,const QVariantMap &selection,QString *error);
}
