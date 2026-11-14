// 层：功能
#include "onnxpredictionservice.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QSet>
#include <cstring>

namespace {
constexpr int maxCursorChars = 4096;
constexpr qint64 cursorTtlMs = 30 * 60 * 1000;
constexpr auto contract = "paleo.local_models.page.v1";
QJsonObject failure(const char *code) {
  return {{"contract",contract},{"ok",false},{"code",code},
          {"metadataOnly",true},{"requires_prepare",true},{"runtimeVerified",false}};
}
QString digest(const QString &value) {
  return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(),QCryptographicHash::Sha256).toHex());
}
QByteArray mac(const QByteArray &payload,const QByteArray &key) {
  return QMessageAuthenticationCode::hash(payload,key,QCryptographicHash::Sha256).toHex();
}
bool equalMac(const QByteArray &a,const QByteArray &b) {
  if(a.size()!=b.size())return false;
  unsigned char difference=0;
  for(qsizetype i=0;i<a.size();++i)difference|=static_cast<unsigned char>(a[i]^b[i]);
  return difference==0;
}
QString sign(const QJsonObject &payload,const QByteArray &key) {
  const auto bytes=QJsonDocument(payload).toJson(QJsonDocument::Compact);
  return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals)+'.'+mac(bytes,key));
}
bool readCursor(const QString &cursor,const QByteArray &key,QJsonObject &payload) {
  // Bound the original string before any encoding, decoding, or JSON allocation.
  if(cursor.isEmpty() || cursor.size()>maxCursorChars)return false;
  const auto dot=cursor.indexOf(QLatin1Char('.'));
  if(dot<1 || dot!=cursor.lastIndexOf(QLatin1Char('.')) || cursor.size()-dot-1!=64)return false;
  for(qsizetype i=0;i<cursor.size();++i) {
    const ushort c=cursor[i].unicode();
    if(i==dot)continue;
    if(i<dot) {if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'))return false;}
    else if(!((c>='a'&&c<='f')||(c>='0'&&c<='9')))return false;
  }
  const auto encoded=cursor.left(dot).toLatin1();
  const auto bytes=QByteArray::fromBase64(encoded,QByteArray::Base64UrlEncoding|QByteArray::AbortOnBase64DecodingErrors);
  if(bytes.isEmpty() || bytes.toBase64(QByteArray::Base64UrlEncoding|QByteArray::OmitTrailingEquals)!=encoded ||
     !equalMac(mac(bytes,key),cursor.mid(dot+1).toLatin1()))return false;
  QJsonParseError error;const auto document=QJsonDocument::fromJson(bytes,&error);
  if(error.error!=QJsonParseError::NoError || !document.isObject())return false;
  payload=document.object();return true;
}
qsizetype stringChars(const QJsonValue &value) {
#if QT_VERSION >= QT_VERSION_CHECK(6,10,0)
  return value.toStringView().size();
#else
  // Qt <6.10 has no non-allocating JSON string view. Registry manifests are
  // independently byte bounded; no UTF-8 conversion/serialization happens here.
  return value.toString().size();
#endif
}
bool smallString(const QJsonValue &value,int maxChars=512) {
  if(!value.isString())return false;
  // UTF-8 views may contain three bytes per UTF-16 code unit. Check their
  // allocation-free upper bound before converting a short value to QString.
  if(stringChars(value)>3*maxChars)return false;
  return value.toString().size()<=maxChars;
}
bool boundedFeatures(const QJsonArray &features) {
  if(features.size()>64)return false;
  const QSet<QString> fields{"version","name","feature","kind","unit","domain","preprocessing","dtype"};
  qsizetype characters=0;
  for(const auto &value:features) {
    if(!value.isObject())return false;
    const auto feature=value.toObject();if(feature.size()>fields.size())return false;
    for(auto it=feature.constBegin();it!=feature.constEnd();++it) {
      if(it.key().size()>32 || !fields.contains(it.key()))return false;
      if(it.key()==QLatin1String("version")) {
        const auto number=it.value().toInteger(-1);
        if(!it.value().isDouble() || number<0 || number>1024)return false;
      } else if(!smallString(it.value()))return false;
      characters+=it.key().size()+stringChars(it.value())+8;
      if(characters>8192)return false;
    }
  }
  // Only structurally bounded, whitelisted declarations reach serialization.
  return QJsonDocument(features).toJson(QJsonDocument::Compact).size()<=8192;
}
bool boundedEntry(const ModelRegistryEntry &entry,QJsonObject &out) {
  if(entry.name.size()>256 || entry.version.size()>256 || entry.task.size()>256 || entry.dataType.size()>256 ||
     entry.inputName.size()>256 || entry.inputDtype.size()>64 || entry.outputSemantics.size()>128 ||
     entry.sha256Pinned.size()>64 || entry.inputShape.size()>16 || !boundedFeatures(entry.features))return false;
  QJsonArray shape;for(const auto value:entry.inputShape)shape.append(qint64(value));
  QJsonArray issues;
  if(entry.featureContractVersion!=1)issues.append("feature_contract_version_unsupported");
  if(entry.features.isEmpty())issues.append("model_feature_contract_unavailable");
  bool pinned=entry.sha256Pinned.size()==64;
  for(const auto c:entry.sha256Pinned)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))pinned=false;
  if(!pinned)issues.append("model_fingerprint_unpinned");
  out={{"name",entry.name},{"version",entry.version},{"task",entry.task},{"dataType",entry.dataType},
       {"weightsSHA",pinned?QJsonValue(entry.sha256Pinned):QJsonValue()},
       {"input",QJsonObject{{"name",entry.inputName},{"dtype",entry.inputDtype},{"shape",shape},
         {"featureContractVersion",entry.featureContractVersion},{"features",entry.features}}},
       {"outputs",QJsonObject{{"semantics",entry.outputSemantics},{"classes",entry.classes}}},
       {"metadataOnly",true},{"requires_prepare",true},{"runtimeVerified",false},
       {"runtimeAvailability","requires_prepare"},{"availabilityReason","runtime_validation_not_performed"},
       {"declarationIssues",issues}};
  return true;
}
bool decimal(const QJsonValue &value,quint64 &number) {
  if(!value.isString() || !smallString(value,20))return false;
  const auto string=value.toString();bool ok=false;number=string.toULongLong(&ok);
  return ok && QString::number(number)==string;
}
QJsonObject cursorPayload(const QString &scope,quint64 epoch,const QString &name,const QString &after,
                          int size,int bytes,qint64 expires) {
  return {{"v",1},{"scope",scope},{"epoch",QString::number(epoch)},{"name",name},
          {"after",after},{"size",size},{"bytes",bytes},{"expires",QString::number(expires)}};
}
}

QJsonObject PaleoOnnxService::modelContractPage(const QString &name,const QString &cursor,
                                               const QString &trustedScope,int pageSize,int maxBytes) {
  if(name.size()>256 || (!name.isEmpty() && !ModelRegistry::isSafeModelName(name)))return failure("invalid_model_name");
  if(trustedScope.isEmpty() || trustedScope.size()>2048)return failure("query_scope_unavailable");
  if(pageSize<1 || pageSize>64 || maxBytes<1024 || maxBytes>32768)return failure("invalid_query_budget");
  if(cursor.size()>maxCursorChars)return failure("cursor_invalid");
  QMutexLocker lock(&m_mutex);
  if(!m_registryActive || !m_registryError.isEmpty())return failure("model_registry_unavailable");
  if(m_modelContractCursorKey.isEmpty()) {
    m_modelContractCursorKey.resize(32);
    for(int i=0;i<32;i+=4) {const auto value=QRandomGenerator::system()->generate();std::memcpy(m_modelContractCursorKey.data()+i,&value,4);}
  }
  const auto scope=digest(trustedScope);
  const auto now=m_modelContractClock?m_modelContractClock():QDateTime::currentMSecsSinceEpoch();
  QString after;
  if(!cursor.isEmpty()) {
    QJsonObject payload;quint64 epoch=0,expires=0;
    if(!readCursor(cursor,m_modelContractCursorKey,payload) || payload.size()!=8 || payload.value("v")!=QJsonValue(1) ||
       !decimal(payload.value("epoch"),epoch) || !decimal(payload.value("expires"),expires) ||
       !smallString(payload.value("after"),256) || payload.value("after").toString().isEmpty())return failure("cursor_invalid");
    if(payload.value("scope")!=QJsonValue(scope))return failure("cursor_scope_mismatch");
    if(epoch!=m_modelConfigEpoch)return failure("cursor_stale");
    if(payload.value("name")!=QJsonValue(name) || payload.value("size")!=QJsonValue(pageSize) ||
       payload.value("bytes")!=QJsonValue(maxBytes))return failure("cursor_query_mismatch");
    if(expires<=quint64(qMax<qint64>(0,now)))return failure("cursor_expired");
    after=payload.value("after").toString();
  }
  const qsizetype matched=name.isEmpty()?m_registryOk.size():(m_registryOk.contains(name)?1:0);
  QJsonObject result{{"contract",contract},{"ok",true},{"topic","local_models"},{"entryFilter","manifest_status_ok"},
    {"count",qint64(m_registryOk.size())},{"matchedCount",qint64(matched)},{"rejectedCount",qint64(m_registryRejectedCount)},
    {"models",QJsonArray()},{"returnedCount",0},{"hasMore",false},{"nextCursor",QString()},
    {"metadataOnly",true},{"requires_prepare",true},{"runtimeVerified",false}};
  auto it=after.isEmpty()?(name.isEmpty()?m_registryOk.constBegin():m_registryOk.constFind(name)):m_registryOk.upperBound(after);
  QJsonArray models;
  while(it!=m_registryOk.constEnd() && (name.isEmpty() || it.key()==name) && models.size()<pageSize) {
    QJsonObject entry;if(!boundedEntry(it.value(),entry))return failure("model_metadata_budget_or_fields_exceeded");
    const auto key=it.key();++it;
    const bool more=it!=m_registryOk.constEnd() && (name.isEmpty() || it.key()==name);
    auto candidate=models;candidate.append(entry);
    auto page=result;page.insert("models",candidate);page.insert("returnedCount",candidate.size());page.insert("hasMore",more);
    page.insert("nextCursor",more?sign(cursorPayload(scope,m_modelConfigEpoch,name,key,pageSize,maxBytes,now+cursorTtlMs),m_modelContractCursorKey):QString());
    page.insert("coverage",QJsonObject{{"complete",!more},{"hasMore",more},{"returnedCount",candidate.size()},{"totalCount",qint64(matched)}});
    if(QJsonDocument(page).toJson(QJsonDocument::Compact).size()>maxBytes) {
      if(models.isEmpty())return failure("model_entry_byte_budget_exceeded");
      break;
    }
    models=candidate;result=page;
  }
  if(models.isEmpty())result.insert("coverage",QJsonObject{{"complete",true},{"hasMore",false},{"returnedCount",0},{"totalCount",qint64(matched)}});
  return result;
}
