// 层：数据
#pragma once

#include "lasdoc.h"
#include <QByteArray>
#include <memory>

namespace paleo::lascache_detail {
// v2 stores binary64 columns. v3 sets this flag and adds a format byte per column.
constexpr quint16 TaggedColumns = 1u << 1;
QByteArray encodeDoc(const QString &fingerprint, const LasDoc &doc);
bool decodeDoc(const QByteArray &payload, const QString &fingerprint, quint16 flags,
               std::shared_ptr<LasDoc> *out);
}
