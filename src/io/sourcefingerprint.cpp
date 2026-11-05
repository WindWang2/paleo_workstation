// 层：数据
#include "sourcefingerprint.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

namespace paleo::document {
FileStamp sourceFileStamp(const QString &path) {
    const QFileInfo file(path);
    return file.isFile()?FileStamp{file.size(),file.lastModified().toMSecsSinceEpoch(),
        file.metadataChangeTime().toMSecsSinceEpoch(),file.birthTime().toMSecsSinceEpoch()}:FileStamp{};
}
bool verifySourceFingerprint(const QString &path,const SourceFingerprint &expected,FileStamp &stamp,
    qint64 &bytesRead,QString *error,const std::function<bool()> &cancelled) {
    if(error)error->clear();bytesRead=0;stamp=sourceFileStamp(path);
    const auto fail=[&](const QString &why){if(error)*error=why;return false;};
    const auto stopped=[&]{return cancelled && cancelled();};
    if(!expected.valid()) return fail(QStringLiteral("来源没有可核验的身份指纹"));
    if(stopped()) return fail(QStringLiteral("已取消"));
    QFile file(path);if(!file.open(QIODevice::ReadOnly)) return fail(file.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if(expected.kind==FingerprintKind::SegyQuickV1) {
        const auto header=file.read(65536);bytesRead=header.size();hash.addData(header);
        hash.addData(QByteArray::number(file.size()));hash.addData(QByteArray::number(stamp.modified));
    } else {
        QByteArray block(1024*1024,Qt::Uninitialized);
        for(;;) {
            if(stopped()) return fail(QStringLiteral("已取消"));
            const auto count=file.read(block.data(),block.size());
            if(count<0) return fail(file.errorString());if(!count) break;
            bytesRead+=count;hash.addData(QByteArrayView(block.constData(),count));
        }
    }
    if(file.error()!=QFile::NoError) return fail(file.errorString());
    if(stopped()) return fail(QStringLiteral("已取消"));
    if(stamp!=sourceFileStamp(path)) return fail(QStringLiteral("核验期间来源发生变化"));
    if(QString::fromLatin1(hash.result().toHex())!=expected.value) return fail(QStringLiteral("来源文件身份不符"));
    return true;
}
}
