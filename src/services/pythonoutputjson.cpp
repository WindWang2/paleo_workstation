// 层：数据
#include "pythonoutputjson.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QStringDecoder>

namespace paleo::python {
namespace {
class UniqueKeys {
  const QByteArray &data;qsizetype offset=0;int nodes=0,maximum;
  void space(){while(offset<data.size()&&(data[offset]==' '||data[offset]=='\t'||data[offset]=='\n'||data[offset]=='\r'))++offset;}
  QByteArray string(){const auto begin=offset++;while(offset<data.size()){
    if(data[offset]=='\\'){offset+=2;continue;}if(data[offset++]=='"')return data.mid(begin,offset-begin);}return {};}
  bool value(int depth){
    if(depth>32||++nodes>maximum)return false;space();if(offset>=data.size())return false;const auto token=data[offset];
    if(token=='{') {++offset;QSet<QString> names;space();if(offset<data.size()&&data[offset]=='}'){++offset;return true;}
      for(;;){space();if(offset>=data.size()||data[offset]!='"')return false;const auto raw=string();
        const auto name=QJsonDocument::fromJson('['+raw+']').array().first().toString();
        if(name.toUtf8().size()>256||names.contains(name))return false;names.insert(name);space();
        if(offset>=data.size()||data[offset++]!=':'||!value(depth+1))return false;space();if(offset>=data.size())return false;
        if(data[offset]=='}'){++offset;return true;}if(data[offset++]!=',')return false;}
    }
    if(token=='['){++offset;space();if(offset<data.size()&&data[offset]==']'){++offset;return true;}
      for(;;){if(!value(depth+1))return false;space();if(offset>=data.size())return false;if(data[offset]==']'){++offset;return true;}if(data[offset++]!=',')return false;}}
    if(token=='"')return !string().isEmpty();
    const auto begin=offset;while(offset<data.size()&&data[offset]!=','&&data[offset]!=']'&&data[offset]!='}'&&data[offset]!=' '&&data[offset]!='\n'&&data[offset]!='\r'&&data[offset]!='\t')++offset;
    return offset>begin;
  }
public:
  UniqueKeys(const QByteArray &bytes,int limit):data(bytes),maximum(limit){}
  bool valid(){const bool ok=value(0);space();return ok&&offset==data.size();}
};
}
QString parsePythonOutputObject(const QByteArray &bytes,qint64 maximum,int nodes,QJsonObject &object) {
  object={};if(maximum<1 || nodes<1 || nodes>16384 || bytes.isEmpty() || bytes.size()>maximum)return "python_output_budget_exceeded";
  QStringDecoder decoder(QStringDecoder::Utf8,QStringConverter::Flag::Stateless);const QString decoded=decoder(bytes);Q_UNUSED(decoded);
  if(decoder.hasError())return "python_output_encoding_invalid";
  QJsonParseError parse;const auto document=QJsonDocument::fromJson(bytes,&parse);
  if(parse.error!=QJsonParseError::NoError || !document.isObject() || !UniqueKeys(bytes,nodes).valid())return "python_output_invalid";
  object=document.object();return {};
}
}
