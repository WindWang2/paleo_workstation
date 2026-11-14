// 层：数据
#include "wellfaciesinputstore.h"
#include "wellfaciesinputplan_internal.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSaveFile>
#include <QUuid>

namespace paleo::wellfacies {
namespace {
constexpr qint64 maxBytes=40LL*1024*1024;
const QString relativeRoot=QStringLiteral(".paleo/capability-ops/well-inputs/");
bool beneath(const QString &project,const QString &path) {
  const auto root=QFileInfo(project).canonicalFilePath(),actual=QFileInfo(path).canonicalFilePath();
  return !root.isEmpty()&&!actual.isEmpty()&&actual.startsWith(root+"/")&&!QFileInfo(path).isSymLink();
}
bool write(const QString &path,const QByteArray &bytes) {
  QSaveFile file(path);file.setDirectWriteFallback(false);
  if(QFileInfo::exists(path)||!file.open(QIODevice::WriteOnly))return false;
  file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
  return file.write(bytes)==bytes.size()&&file.commit();
}
}
QJsonObject storeInput(const QString &project,const PreparedInput &input,QString &code,const std::function<bool()> &cancelled) {
  code.clear();const auto fail=[&](const QString &reason){code=reason;return QJsonObject{};};
  if(!input.ok||input.segments.isEmpty()||input.segments.size()!=input.requestBodies.size()||
      input.segments.size()!=input.requestHashes.size()||input.segments.size()!=input.segmentOccurrences.size())return fail("invalid_prepared_input");
  const auto relative=relativeRoot+QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto directory=QDir(project).filePath(relative);
  if(!QDir().mkpath(directory)||!beneath(project,directory))return fail("private_input_store_unavailable");
  QJsonArray segments; qint64 size=0;
  const auto facies=QJsonArray::fromVariantList(input.segments.first().facies);
  for(int i=0;i<input.segments.size();++i) {
    if(cancelled&&cancelled())return fail("cancelled");
    const auto &body=input.requestBodies[i];size+=body.size();
    if(QJsonArray::fromVariantList(input.segments[i].facies)!=facies)return fail("inconsistent_frozen_class_schema");
    if(size>maxBytes||!write(directory+"/body-"+QString::number(i)+".json",body))return fail("private_input_store_failed");
    segments.append(QJsonObject{{"requestHash",input.requestHashes[i]},{"bodyHash",detail::hash(body)},
      {"occurrence",input.segmentOccurrences[i]}});
  }
  const QJsonObject manifest{{"contract","paleo.well-facies.stored-input.v2"},{"wellName",input.wellName},
    {"model",frozenModelJson(input.model)},{"source",input.sourceManifest},{"segments",segments},{"facies",facies},
    {"inputHash",input.inputHash},{"featureSchemaHash",input.featureSchemaHash}};
  const auto bytes=QJsonDocument(manifest).toJson(QJsonDocument::Compact);
  if(size+bytes.size()>maxBytes||!write(directory+"/manifest.json",bytes))return fail("private_input_store_failed");
  return {{"directory",relative},{"manifestHash",detail::hash(bytes)}};
}
PreparedInput loadInput(const QString &project,const QJsonObject &reference,const std::function<bool()> &cancelled) {
  const auto fail=[](const QString &code){PreparedInput out;out.code=code;out.error=code;return out;};
  const auto relative=reference.value("directory").toString();
  const auto uuid=relative.mid(relativeRoot.size());
  if(!relative.startsWith(relativeRoot)||uuid.size()!=36||QUuid(uuid).isNull()||relative.contains(".."))return fail("invalid_stored_input_ref");
  const auto directory=QDir(project).filePath(relative);
  if(!beneath(project,directory))return fail("stored_input_unavailable");
  qint64 budget=maxBytes;
  const auto read=[&](const QString &name) {
    if(cancelled&&cancelled())return QByteArray();
    const auto path=directory+"/"+name;QFile file(path);
    if(!beneath(project,path)||!file.open(QIODevice::ReadOnly)||file.size()>budget)return QByteArray();
    auto bytes=file.read(budget+1);
    if(bytes.size()>budget||!file.atEnd())return QByteArray();
    budget-=bytes.size();return bytes;
  };
  const auto bytes=read("manifest.json");
  if(bytes.isEmpty()||detail::hash(bytes)!=reference.value("manifestHash").toString())return fail("stored_input_hash_mismatch");
  const auto manifest=QJsonDocument::fromJson(bytes).object();
  if(manifest.value("contract")!="paleo.well-facies.stored-input.v2")return fail("invalid_stored_input");
  PreparedInput out;out.wellName=manifest.value("wellName").toString();out.inputHash=manifest.value("inputHash").toString();
  out.featureSchemaHash=manifest.value("featureSchemaHash").toString();out.sourceManifest=manifest.value("source").toObject();
  for(const auto &value:out.sourceManifest.value("skipped").toArray())out.skipped.append(value.toString());
  for(const auto &value:out.sourceManifest.value("occurrences").toArray()) {
    const auto window=value.toObject();out.occurrences.append({window.value("key").toString(),window.value("depthType").toString(),
      window.value("sourceDepthUnit").toString(),window.value("topMdMetres").toDouble(qQNaN()),
      window.value("bottomMdMetres").toDouble(qQNaN()),window.value("derivedDivisionGeometry").toBool()});
  }
  for(const auto &value:out.sourceManifest.value("curves").toArray()) {
    const auto curve=value.toObject();out.curves.append({curve.value("feature").toString(),curve.value("unit").toString(),
      curve.value("sourceFingerprint").toString(),curve.value("curveId").toString().toLongLong(),
      curve.value("sourceId").toString().toLongLong(),curve.value("logSetId").toString().toLongLong(),
      curve.value("sourceDepthType").toString(),curve.value("sourceDepthUnit").toString(),curve.value("mdReference").toString(),
      curve.value("sourceName").toString()});
  }
  if(!frozenModelFromJson(manifest.value("model").toObject(),out.model))return fail("invalid_stored_model");
  const auto segments=manifest.value("segments").toArray();
  if(segments.isEmpty()||segments.size()>64)return fail("invalid_stored_segments");
  for(int i=0;i<segments.size();++i) {
    const auto segment=segments[i].toObject();const auto bodyBytes=read("body-"+QString::number(i)+".json");
    if(bodyBytes.isEmpty()||detail::hash(bodyBytes)!=segment.value("bodyHash").toString())return fail("stored_input_hash_mismatch");
    const auto body=QJsonDocument::fromJson(bodyBytes).object();const auto wells=body.value("wells").toArray();
    if(body.value("modelVersionId").toString()!=out.model.id||wells.size()!=1||
       wells.first().toObject().value("wellName").toString()!=out.wellName)return fail("invalid_stored_input");
    WellFaciesInput input;input.rows=wells.first().toObject().value("rows").toArray();
    const auto facies=manifest.value("facies").toArray();
    QString error;input.modelLabels=out.model.labels;input.facies=facies.toVariantList();QVariantList checked;
    if(!input.ready()||!resolveWellFaciesClasses(input,out.model.labels,&checked,&error)||
       QJsonArray::fromVariantList(checked)!=facies)return fail("invalid_stored_input");
    input.facies=checked;
    out.segments.append(input);out.requestBodies.append(bodyBytes);out.requestHashes.append(segment.value("requestHash").toString());
    out.segmentOccurrences.append(segment.value("occurrence").toString());
  }
  out.ok=true;return out;
}
namespace {
QString resultPath(const QString &project,const QJsonObject &reference,int segment,const QString &hash) {
  if(segment<0||segment>=64||hash.size()!=64)return {};
  for(const auto &ch:hash)if(!QStringLiteral("0123456789abcdef").contains(ch))return {};
  const auto relative=reference.value("directory").toString(),uuid=relative.mid(relativeRoot.size());
  if(!relative.startsWith(relativeRoot)||uuid.size()!=36||QUuid(uuid).isNull())return {};
  const auto directory=QDir(project).filePath(relative);if(!beneath(project,directory))return {};
  QFile manifest(directory+"/manifest.json");
  if(!beneath(project,manifest.fileName())||!manifest.open(QIODevice::ReadOnly)||manifest.size()>maxBytes)return {};
  const auto bytes=manifest.read(maxBytes+1);
  if(bytes.size()>maxBytes||detail::hash(bytes)!=reference.value("manifestHash").toString())return {};
  return directory+"/result-"+QString::number(segment)+"-"+hash+".json";
}
}
bool storeResultCache(const QString &project,const QJsonObject &reference,int segment,const QByteArray &bytes,QString &code) {
  code.clear();if(bytes.isEmpty()||bytes.size()>32*1024*1024){code="external_result_budget_exceeded";return false;}
  const auto hash=detail::hash(bytes),path=resultPath(project,reference,segment,hash);
  if(path.isEmpty()){code="invalid_stored_input_ref";return false;}
  if(QFileInfo::exists(path)) {
    const auto existing=loadResultCache(project,reference,segment,hash,code);return existing==bytes&&!existing.isEmpty();
  }
  if(!write(path,bytes)){code="external_result_cache_failed";return false;}return true;
}
QByteArray loadResultCache(const QString &project,const QJsonObject &reference,int segment,const QString &hash,QString &code) {
  code.clear();const auto path=resultPath(project,reference,segment,hash);QFile file(path);
  if(path.isEmpty()||!beneath(project,path)||!file.open(QIODevice::ReadOnly)){code="external_result_cache_unavailable";return {};}
  if(file.size()>32*1024*1024){code="external_result_budget_exceeded";return {};}
  const auto bytes=file.read(32*1024*1024+1);
  if(bytes.size()>32*1024*1024||detail::hash(bytes)!=hash){code="external_result_cache_mismatch";return {};}
  return bytes;
}
} // namespace paleo::wellfacies
