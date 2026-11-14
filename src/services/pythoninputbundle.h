// 层：数据
#pragma once
#include "pythonruntime.h"
#include <QMap>

namespace paleo::python {
struct InputBundle {
  QString code, root;
  QJsonObject reference, manifest;
  bool ok() const { return code==QLatin1String("ok"); }
};
// Private project snapshots. Metadata contains exact versions/units, never authority.
InputBundle freezeInputBundle(const QString &projectDir,const QMap<QString,QByteArray> &files,
    const QJsonObject &metadata,const ExecutionPolicy &policy,const std::function<bool()> &cancelled = {});
InputBundle verifyInputBundle(const QString &projectDir,const QJsonObject &reference,
    const ExecutionPolicy &policy,const std::function<bool()> &cancelled = {});
} // namespace paleo::python
