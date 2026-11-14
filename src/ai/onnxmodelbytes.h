// 层：功能
#pragma once
#include <QByteArray>
#include <QString>
namespace paleo::onnx {
inline constexpr qint64 maxModelBytes=512LL*1024*1024;
QString boundedFingerprint(const QString &path,QString *error);
QByteArray readPinnedBytes(const QString &path,const QString &expectedSha256,QString *error);
}
