// 层：数据
#pragma once
#include "domain/documentsource.h"
#include <functional>
namespace paleo::document {
FileStamp sourceFileStamp(const QString &path);
bool verifySourceFingerprint(const QString &path,const SourceFingerprint &expected,FileStamp &stamp,
    qint64 &bytesRead,QString *error,const std::function<bool()> &cancelled={});
}
