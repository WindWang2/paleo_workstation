// 层：数据
#include "volume.h"
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QtEndian>
#include <limits>
#include <cstring>

namespace paleo::inversion {
namespace {
constexpr qint64 reservedHeader=65536;
bool fail(QString* error,const QString& text){if(error)*error=text;return false;}
qint64 bodyBytes(const QJsonObject& h){
 qint64 count=1;for(const auto& key:{"nIl","nXl","nS"}){
  const qint64 n=h.value(key).toInteger(-1);if(n<=0||count>std::numeric_limits<qint64>::max()/n)return -1;count*=n;
 }return count>std::numeric_limits<qint64>::max()/4?-1:count*4;
}
bool writeHeader(QSaveFile& file,const QJsonObject& header,QString* error){
 const auto json=QJsonDocument(header).toJson(QJsonDocument::Compact);
 if(json.size()>reservedHeader)return fail(error,QStringLiteral("阻抗体头超过保留预算"));
 QByteArray bytes("IIMP1\n",6);const quint32 size=qToLittleEndian(quint32(reservedHeader));bytes.append(reinterpret_cast<const char*>(&size),4);
 bytes.append(json);bytes.append(QByteArray(reservedHeader-json.size(),' '));
 if(!file.seek(0)||file.write(bytes)!=bytes.size())return fail(error,QStringLiteral("阻抗体头写入失败：%1").arg(file.errorString()));
 return true;
}
}
bool readImpedanceVolumeHeader(const QString& path,QJsonObject* header,QString* error){
 if(error)error->clear();if(header)*header={};QFile file(path);
 if(!file.open(QIODevice::ReadOnly))return fail(error,file.errorString());
 const auto prefix=file.read(10);if(prefix.size()!=10||prefix.left(6)!="IIMP1\n")return fail(error,QStringLiteral("IIMP1 体头无效"));
 const auto size=qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(prefix.constData()+6));
 if(size>16*1024*1024||qint64(size)>file.size()-10)return fail(error,QStringLiteral("IIMP1 体头长度无效"));
 QJsonParseError parse;const auto json=QJsonDocument::fromJson(file.read(size),&parse);
 if(parse.error!=QJsonParseError::NoError||!json.isObject())return fail(error,QStringLiteral("IIMP1 体头解析失败"));
 const auto expected=bodyBytes(json.object());
 if(expected<0||file.size()-10-size!=expected)return fail(error,QStringLiteral("IIMP1 体大小与声明不符"));
 if(header)*header=json.object();return true;
}
ImpedanceVolumeStream::ImpedanceVolumeStream(const QString& path):m_file(std::make_unique<QSaveFile>(path)){}
ImpedanceVolumeStream::~ImpedanceVolumeStream()=default;
bool ImpedanceVolumeStream::open(const QJsonObject& header,QString* error){
 m_expected=bodyBytes(header);m_written=0;
 if(m_expected<0)return fail(error,QStringLiteral("阻抗体维度无效或溢出"));
 if(!m_file->open(QIODevice::WriteOnly))return fail(error,m_file->errorString());
 return writeHeader(*m_file,header,error);
}
bool ImpedanceVolumeStream::append(const float* values,qint64 count,QString* error){
 if(!m_file->isOpen()||count<0||count>(m_expected-m_written)/4||(count>0&&!values))return fail(error,QStringLiteral("阻抗体块长度无效"));
 const qint64 bytes=count*4;
#if Q_BYTE_ORDER == Q_BIG_ENDIAN
 QByteArray converted(bytes,Qt::Uninitialized);
 for(qint64 i=0;i<count;++i){quint32 bits;std::memcpy(&bits,values+i,4);qToLittleEndian(bits,reinterpret_cast<uchar*>(converted.data()+i*4));}
 const auto written=m_file->write(converted);
#else
 const auto written=m_file->write(reinterpret_cast<const char*>(values),bytes);
#endif
 if(written!=bytes)return fail(error,QStringLiteral("阻抗体块写入失败：%1").arg(m_file->errorString()));
 m_written+=bytes;return true;
}
bool ImpedanceVolumeStream::finish(const QJsonObject& header,QString* error){
 if(m_written!=m_expected||bodyBytes(header)!=m_expected)return fail(error,QStringLiteral("阻抗体未完整写入或维度改变"));
 if(!writeHeader(*m_file,header,error))return false;
 if(!m_file->commit())return fail(error,m_file->errorString());return true;
}
}
