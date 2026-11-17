// 层：数据
#include "piprotocol.h"
#include <QJsonDocument>
#include <QStringDecoder>
#include <QtEndian>
#include <algorithm>

namespace paleo::pi {
QByteArray encodeFrame(const QJsonObject &value) {
  const auto body=QJsonDocument(value).toJson(QJsonDocument::Compact);
  if(body.isEmpty()||body.size()>MaxFrameBytes)return {};
  QByteArray frame(4,Qt::Uninitialized);qToLittleEndian<quint32>(quint32(body.size()),frame.data());
  frame+=body;return frame;
}
bool takeFrame(QByteArray &buffer,QJsonObject &result,bool &complete,QString &error) {
  result={};complete=false;error.clear();
  const auto fail=[&](const char *code){buffer.clear();error=QLatin1String(code);return false;};
  if(buffer.size()>MaxFrameBytes+4)return fail("pi_frame_budget");
  if(buffer.size()<4)return true;
  const auto length=qFromLittleEndian<quint32>(buffer.constData());
  if(!length||length>MaxFrameBytes)return fail("pi_frame_budget");
  if(buffer.size()<qsizetype(length)+4)return true;
  const auto body=buffer.mid(4,length);
  QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless);
  const QString text=decoder(body);Q_UNUSED(text);
  if(decoder.hasError())return fail("pi_frame_encoding");
  QJsonParseError parse;const auto document=QJsonDocument::fromJson(body,&parse);
  if(parse.error!=QJsonParseError::NoError||!document.isObject()||
      document.toJson(QJsonDocument::Compact)!=body)return fail("pi_frame_invalid");
  result=document.object();buffer.remove(0,length+4);complete=true;return true;
}
bool validCounter(const QString &text) {
  if(text.isEmpty()||text.size()>20||(text.size()>1&&text.front()==QLatin1Char('0')))return false;
  if(!std::all_of(text.begin(),text.end(),[](QChar c){return c>=QLatin1Char('0')&&c<=QLatin1Char('9');}))return false;
  bool ok=false;const auto value=text.toULongLong(&ok);return ok&&QString::number(value)==text;
}
bool validSession(const QString &text) {
  return !text.isEmpty()&&text.size()<=128&&std::all_of(text.begin(),text.end(),[](QChar c) {
    return (c>=QLatin1Char('a')&&c<=QLatin1Char('z'))||(c>=QLatin1Char('A')&&c<=QLatin1Char('Z'))||
      (c>=QLatin1Char('0')&&c<=QLatin1Char('9'))||c==QLatin1Char('_')||c==QLatin1Char('-');
  });
}
}
