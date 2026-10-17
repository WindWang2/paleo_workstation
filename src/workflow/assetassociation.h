// 层：功能
#pragma once
#include <QString>
#include <QtGlobal>
class DataCatalog;
namespace paleo::assetassociation {
struct Change {
  QString assetId, entityId, role;
  quint64 expectedSequence = 0;
  int sourceLink = -1; // -1 添加；其他值更改这条现有关联。
  bool primary = false;
};
bool apply(DataCatalog *catalog, const Change &change, QString *error = nullptr);
}
