// 层：功能
#include "onnxmodelbytes.h"
#include <QCryptographicHash>
#include <QFile>
namespace paleo::onnx {
QString boundedFingerprint(const QString &path,QString *error) {
  const auto fail=[&](const QString &code){if(error)*error=code;return QString{};};
  QFile file(path);if(!file.open(QIODevice::ReadOnly))return fail("model_bytes_unavailable");
  if(file.size()<1 || file.size()>maxModelBytes)return fail("model_byte_budget_exceeded");
  QCryptographicHash hash(QCryptographicHash::Sha256);qint64 remaining=maxModelBytes;
  while(!file.atEnd()) {
    const auto chunk=file.read(1024*1024);
    if(chunk.isEmpty() && file.error()!=QFile::NoError)return fail("model_bytes_unavailable");
    if(chunk.size()>remaining)return fail("model_byte_budget_exceeded");remaining-=chunk.size();hash.addData(chunk);
  }
  return QString::fromLatin1(hash.result().toHex());
}
QByteArray readPinnedBytes(const QString &path,const QString &expectedSha256,QString *error) {
  const auto fail=[&](const QString &code){if(error)*error=code;return QByteArray{};};
  QFile file(path);if(!file.open(QIODevice::ReadOnly))return fail("model_bytes_unavailable");
  if(file.size()<1 || file.size()>maxModelBytes)return fail("model_byte_budget_exceeded");
  QByteArray bytes;QCryptographicHash hash(QCryptographicHash::Sha256);
  while(!file.atEnd()) {
    auto chunk=file.read(1024*1024);
    if(chunk.isEmpty() && file.error()!=QFile::NoError)return fail("model_bytes_unavailable");
    if(chunk.size()>maxModelBytes-bytes.size())return fail("model_byte_budget_exceeded");
    hash.addData(chunk);bytes.append(chunk);
  }
  if(QString::fromLatin1(hash.result().toHex())!=expectedSha256.toLower())return fail("model_bytes_changed");
  return bytes;
}
}
