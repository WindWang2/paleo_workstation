// 层：数据
#pragma once
#include "nativemodel.h"
#include <QVariantMap>
namespace paleo::data { class DataService; struct ParsedFile; }
namespace paleo::native_model {
// Open only a committed, hash-verified immutable version using a thread-local
// read-only service. Bindings never expose SQLite connections to callers.
Read open(const QString &path, const QString &kind, paleo::data::DataService &service,
          QVariantMap &binding, QString *error);
void preserveWellColumns(paleo::data::ParsedFile &model,
                         const dataimport_detail::FamilyContext &ctx);
}
