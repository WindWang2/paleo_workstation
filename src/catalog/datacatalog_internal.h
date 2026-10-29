// 层：数据
#pragma once

#include "datacatalog.h"

#include <QStringList>
#include <QVariant>

// DataCatalog TU 族（_assets / _versions / _links / _entities / _derivation /
// _export）共享的实现细节。对外不可见——公共 API 只看 datacatalog.h；
// 同族先例见 metadata/storeerrors_internal.h 与 catalogstore_internal.h。
namespace paleo::catalog_detail
{

// 图版证据引用（版本 extra 的 retained_by_map_results）词元读法：
// QStringList 与 JSON 重载来的 QVariantList 都算，空串词元丢弃。
// 空读不得让被引用的版本穿过 purge / 治理删除闸门——「没有引用」与
// 「读不到引用」在这两条删除路径上必须给出同一个答案（拒绝）。
inline QStringList mapResultRetention(const QVariant &value)
{
  QStringList tokens = value.toStringList();
  if (!tokens.isEmpty())
    return tokens;
  for (const auto &entry : value.toList())
  {
    const auto token = entry.toString();
    if (!token.isEmpty())
      tokens << token;
  }
  return tokens;
}

} // namespace paleo::catalog_detail
