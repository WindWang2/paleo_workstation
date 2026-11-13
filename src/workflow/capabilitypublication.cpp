// 层：功能
// workflow/capabilitypublication.cpp — 冻结输出契约/身份编解码与 worker 文件复核。
#include "capabilitypublication_internal.h"
#include "metadata/managedpath.h"
#include "catalog/datacatalog.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QFile>
#include <QCryptographicHash>
#include <QUuid>
#include <limits>

namespace paleo::capability::publication_detail {
DerivedStaging reserveTarget(DataCatalog *catalog,const QString &root,const QString &type,const QString &name,
                             const QString &fileName,QHash<QString,DerivedStaging> &reservations,QString *error) {
  const auto fail=[&](const QString &code){if(error)*error=code;return DerivedStaging{};};
  if(!catalog || !catalog->isOpen())return fail("catalog_unavailable");
  if(!DataCatalog::isSafePathSegment(fileName))return fail("unsafe_output_name");
  const auto key=QString::fromUtf8(QJsonDocument(QJsonArray{type,name}).toJson(QJsonDocument::Compact));
  const auto previous=reservations.value(key);DerivedStaging staged;
  if(!previous.assetId.isEmpty())staged=previous;
  else {
    for(const auto &asset:catalog->assets())if(asset.type==type && asset.displayName==name){staged.assetId=asset.id;break;}
    if(staged.assetId.isEmpty()) {
      staged.assetId="ast-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
      staged.reservedType=type;staged.reservedDisplayName=name;staged.reservedFormat=QFileInfo(fileName).suffix().toLower();
    }
  }
  const auto current=catalog->currentVersion(staged.assetId);
  const int maximum=qMax(current.id.isEmpty()?0:current.versionNumber,previous.versionNumber);
  if(maximum==std::numeric_limits<int>::max())return fail("version_number_exhausted");
  staged.versionId="ver-"+QUuid::createUuid().toString(QUuid::WithoutBraces);staged.versionNumber=maximum+1;
  staged.expectedVersionId=current.id;
  staged.relativePath="artifacts/"+DataCatalog::managedPath("derived",staged.assetId,staged.versionId,fileName);
  staged.absolutePath=managedpath::resolveManagedPath(root,staged.relativePath);
  if(staged.absolutePath.isEmpty() || !QDir().mkpath(QFileInfo(staged.absolutePath).absolutePath()))return fail("stage_failed");
  reservations.insert(key,staged);return staged;
}
namespace {
DerivedFileFingerprint fingerprintBounded(const QString &path,qint64 maxBytes,QString *error,const std::function<bool()> &cancelled) {
  DerivedFileFingerprint result;const QFileInfo before(path);QFile file(path);
  const auto fail=[&](const QString &code){if(error)*error=code;return DerivedFileFingerprint{};};
  if(!before.isFile() || before.isSymLink() || before.size()<0 || before.size()>maxBytes || !file.open(QIODevice::ReadOnly))return fail("artifact_budget_exceeded");
  QCryptographicHash hash(QCryptographicHash::Sha256);qint64 consumed=0;
  while(!file.atEnd()) {
    if(cancelled && cancelled())return fail("cancelled");
    const auto bytes=file.read(qMin<qint64>(1024*1024-1,maxBytes-consumed)+1);
    if(bytes.size()>maxBytes-consumed)return fail("artifact_budget_exceeded");
    if(bytes.isEmpty())return fail("artifact_read_failed");
    consumed+=bytes.size();hash.addData(bytes);
  }
  result.path=before.absoluteFilePath();result.size=before.size();result.modified=before.lastModified();
  result.sha256=QString::fromLatin1(hash.result().toHex());
  if(consumed!=result.size || !result.matches(path))return fail("artifact_changed");return result;
}
QJsonObject stagingJson(const DerivedStaging &st) {
  return {{"path", st.relativePath}, {"assetId", st.assetId}, {"versionId", st.versionId},
      {"versionNumber", st.versionNumber}, {"expectedHead", st.expectedVersionId},
      {"reservedType", st.reservedType}, {"reservedName", st.reservedDisplayName}, {"reservedFormat", st.reservedFormat}};
}
bool stagingFromJson(const QString &root, const QJsonObject &json, DerivedStaging &st) {
  st.relativePath = json.value("path").toString();
  if (!st.relativePath.startsWith(QLatin1String("artifacts/derived/"))) return false;
  st.absolutePath = paleo::managedpath::resolveManagedPath(root, st.relativePath);
  st.assetId = json.value("assetId").toString(); st.versionId = json.value("versionId").toString();
  st.versionNumber = json.value("versionNumber").toInt(-1); st.expectedVersionId = json.value("expectedHead").toString();
  st.reservedType = json.value("reservedType").toString(); st.reservedDisplayName = json.value("reservedName").toString();
  st.reservedFormat = json.value("reservedFormat").toString();
  return st.isValid() && st.versionNumber > 0 && DataCatalog::isSafePathSegment(st.assetId) &&
         DataCatalog::isSafePathSegment(st.versionId) &&
         st.relativePath == QStringLiteral("artifacts/") + DataCatalog::managedPath("derived", st.assetId,
             st.versionId, QFileInfo(st.absolutePath).fileName());
}
bool verify(const QString &path, const QJsonObject &json, DerivedFileFingerprint &fingerprint,
            qint64 &remaining, QString *error, const std::function<bool()> &cancelled) {
  bool valid = false;
  const auto size = json.value("byteSize").toString().toLongLong(&valid);
  if (!valid || size < 0 || size > remaining || json.value("sha256").toString().size() != 64) {
    if (error) *error = "invalid_artifact_manifest"; return false;
  }
  const QFileInfo actual(path);
  if(!actual.isFile() || actual.size()!=size || actual.size()>remaining){if(error)*error="artifact_changed";return false;}
  remaining -= size;
  fingerprint = fingerprintBounded(path,size,error,cancelled);
  if (fingerprint.size != size || fingerprint.sha256 != json.value("sha256").toString()) {
    if (error && error->isEmpty()) *error = "artifact_changed"; return false;
  }
  return true;
}
}

QJsonObject specJson(const AlgorithmSpec &spec) {
  QJsonArray ports;
  for (const auto &port : spec.outputs) {
    if(port.kind!="raster" && port.kind!="vector" && port.kind!="file" && port.kind!="table")continue;
    ports.append(QJsonObject{{"id", port.id}, {"kind", port.kind}, {"required", port.required},
        {"min", port.effectiveCardinalityMin()}, {"max", port.cardinalityMax}, {"unit", port.unit},
        {"axis", port.axis}, {"datum", port.verticalDatum}, {"geometry", port.geometryHint},
        {"constraints", QJsonObject::fromVariantMap(port.constraints)}});
  }
  return {{"schema", 1}, {"id", spec.id}, {"contractVersion", spec.contractVersion},
      {"implementationVersion", spec.implementationVersion}, {"outputs", ports}};
}

bool specFromJson(const QJsonObject &json, AlgorithmSpec &spec) {
  spec = {};
  if (json.value("schema") != QJsonValue(1)) return false;
  spec.id = json.value("id").toString(); spec.contractVersion = json.value("contractVersion").toInt();
  spec.implementationVersion = json.value("implementationVersion").toString();
  const auto ports = json.value("outputs").toArray();
  if (spec.id.isEmpty() || spec.contractVersion < 1 || spec.implementationVersion.isEmpty() ||
      ports.isEmpty() || ports.size() > 64) return false;
  QSet<QString> ids;
  for (const auto &entry : ports) {
    const auto object = entry.toObject(); AlgoPort port;
    port.id = object.value("id").toString(); port.kind = object.value("kind").toString();
    port.required = object.value("required").toBool(); port.cardinalityMin = object.value("min").toInt(-1);
    port.cardinalityMax = object.value("max").toInt(-1); port.unit = object.value("unit").toString();
    port.axis = object.value("axis").toString(); port.verticalDatum = object.value("datum").toString();
    port.geometryHint = object.value("geometry").toString(); port.constraints = object.value("constraints").toObject().toVariantMap();
    if (!DataCatalog::isSafePathSegment(port.id) || port.cardinalityMax != 1 ||
        port.effectiveCardinalityMin() < 0 || port.effectiveCardinalityMin() > 1 || ids.contains(port.id) ||
        (port.kind != QLatin1String("raster") && port.kind != QLatin1String("vector") &&
         port.kind != QLatin1String("file") && port.kind != QLatin1String("table"))) return false;
    ids.insert(port.id); spec.outputs.append(port);
  }
  spec.complete = true;
  return true;
}

QJsonObject targetsJson(const QVector<AlgorithmOutputTarget> &targets) {
  QJsonArray rows;
  for (const auto &target : targets) rows.append(QJsonObject{{"portId", target.portId}, {"staging", stagingJson(target.staging)}});
  return {{"targets", rows}};
}

bool targetsFromJson(const QString &root, const QJsonObject &json, QVector<AlgorithmOutputTarget> &targets) {
  targets.clear(); QSet<QString> ports;
  const auto rows = json.value("targets").toArray();
  if (rows.isEmpty() || rows.size() > 64) return false;
  for (const auto &entry : rows) {
    const auto row = entry.toObject(); AlgorithmOutputTarget target;
    target.portId = row.value("portId").toString();
    if (!DataCatalog::isSafePathSegment(target.portId) || ports.contains(target.portId) ||
        !stagingFromJson(root, row.value("staging").toObject(), target.staging)) return false;
    ports.insert(target.portId); targets.append(target);
  }
  return true;
}

QJsonObject preparedJson(const DerivedPreparedSet &prepared) {
  QJsonArray members;
  for (const auto &member : prepared.members)
    members.append(QJsonObject{{"portId", member.portId}, {"staging", stagingJson(member.staging)},
        {"sha256", member.fingerprint.sha256}, {"byteSize", QString::number(member.fingerprint.size)},
        {"extra", QJsonObject::fromVariantMap(member.extra)}, {"parents", QJsonArray::fromStringList(member.parentPortIds)}});
  return {{"schema", 2}, {"id", prepared.id}, {"algorithmId", prepared.algorithmId},
      {"documentId", prepared.documentId}, {"operationId", prepared.operationId}, {"requestHash", prepared.requestHash},
      {"planHash", prepared.planHash}, {"storeGeneration", prepared.storeGeneration}, {"members", members}};
}

bool preparedFromJson(const QString &root, const OperationLog::Record &record, DerivedPreparedSet &prepared,
                      QString *error, const std::function<bool()> &cancelled) {
  prepared = {};
  const auto json = record.preparedManifest;
  if (json.value("schema") != QJsonValue(2) || json.value("documentId").toString() != record.documentId ||
      json.value("operationId").toString() != record.operationId || json.value("requestHash").toString() != record.requestHash ||
      json.value("planHash").toString() != record.planHash || json.value("storeGeneration").toString() != record.storeGeneration)
    return false;
  prepared.publicationSchema = 2; prepared.id = json.value("id").toString(); prepared.algorithmId = json.value("algorithmId").toString();
  prepared.documentId = record.documentId; prepared.operationId = record.operationId;
  prepared.requestHash = record.requestHash; prepared.planHash = record.planHash; prepared.storeGeneration = record.storeGeneration;
  QVector<AlgorithmOutputTarget> targets;
  if (!targetsFromJson(root, record.stagePlan, targets) || prepared.id != record.stagePlan.value("outputSetId").toString()) return false;
  const auto members = json.value("members").toArray();
  if (members.size() != targets.size()) return false;
  QSet<QString> ports; qint64 remaining = 1024LL * 1024 * 1024;
  for (const auto &entry : members) {
    const auto object = entry.toObject(); DerivedPreparedMember member;
    member.portId = object.value("portId").toString();
    if (ports.contains(member.portId) || !stagingFromJson(root, object.value("staging").toObject(), member.staging)) return false;
    bool matched = false;
    for (const auto &target : targets)
      matched = matched || (target.portId == member.portId && stagingJson(target.staging) == object.value("staging").toObject());
    if (!matched || !verify(member.staging.absolutePath, object, member.fingerprint, remaining, error, cancelled)) return false;
    member.extra = object.value("extra").toObject().toVariantMap();
    for (const auto &parent : object.value("parents").toArray()) member.parentPortIds.append(parent.toString());
    ports.insert(member.portId); prepared.members.append(member);
  }
  return !prepared.id.isEmpty() && !prepared.algorithmId.isEmpty();
}

QJsonObject artifactManifest(const QString &root, const QString &operationId, const QString &privateDir,
                             const AlgorithmRunResult &outputs, QString *error,
                             const std::function<bool()> &cancelled, const QSet<QString> &allowedManaged) {
  if (!outputs.ok || outputs.artifacts.isEmpty() || outputs.artifacts.size() > 64) return {};
  QJsonArray ports; QSet<QString> ids; qint64 remaining = 1024LL * 1024 * 1024;
  const auto privateRoot = QFileInfo(privateDir).canonicalFilePath();
  const auto expected = QDir(QFileInfo(root).canonicalFilePath()).filePath(
      QStringLiteral(".paleo/capability-ops/") + operationId + '/');
  if (privateRoot.isEmpty() || !privateRoot.startsWith(expected)) return {};
  const auto allowedPrivate = privateRoot + '/';
  for (const auto &artifact : outputs.artifacts) {
    const auto info = QFileInfo(artifact.absolutePath);
    const auto relative = QDir(root).relativeFilePath(info.absoluteFilePath());
    const auto actual = paleo::managedpath::resolveManagedPath(root, relative);
    if (!DataCatalog::isSafePathSegment(artifact.portId) || ids.contains(artifact.portId) || actual.isEmpty() ||
        (!info.canonicalFilePath().startsWith(allowedPrivate) && !allowedManaged.contains(info.canonicalFilePath())) ||
        info.size() < 0 || info.size() > remaining) { if (error) *error = "untrusted_artifact_path"; return {}; }
    const auto fingerprint = fingerprintBounded(actual,info.size(),error,cancelled);
    if (fingerprint.sha256.isEmpty()) return {};
    remaining -= fingerprint.size;
    ports.append(QJsonObject{{"portId", artifact.portId}, {"path", relative}, {"sha256", fingerprint.sha256},
        {"byteSize", QString::number(fingerprint.size)}, {"extra", QJsonObject::fromVariantMap(artifact.extra)}});
    ids.insert(artifact.portId);
  }
  return {{"schema", 1}, {"operationId", operationId}, {"ports", ports}};
}

bool outputsFromManifest(const QString &root, const QString &operationId, const QJsonObject &manifest,
                         AlgorithmRunResult &outputs, QString *error,
                         const std::function<bool()> &cancelled, bool verifyBytes, const QSet<QString> &allowedManaged) {
  outputs = {};
  if (manifest.value("schema") != QJsonValue(1) || manifest.value("operationId").toString() != operationId) return false;
  const auto ports = manifest.value("ports").toArray();
  if (ports.isEmpty() || ports.size() > 64) return false;
  qint64 remaining = 1024LL * 1024 * 1024; QSet<QString> ids;
  for (const auto &entry : ports) {
    const auto object = entry.toObject(); AlgorithmOutputArtifact artifact;
    artifact.portId = object.value("portId").toString();
    const auto relative = object.value("path").toString();
    artifact.absolutePath = paleo::managedpath::resolveManagedPath(root, relative);
    if (artifact.absolutePath.isEmpty()) return false;
    if ((!relative.startsWith(QStringLiteral(".paleo/capability-ops/") + operationId + '/') &&
         !allowedManaged.contains(QFileInfo(artifact.absolutePath).canonicalFilePath())) ||
        ids.contains(artifact.portId) || !DataCatalog::isSafePathSegment(artifact.portId)) return false;
    DerivedFileFingerprint fingerprint;
    if (verifyBytes && !verify(artifact.absolutePath, object, fingerprint, remaining, error, cancelled)) return false;
    artifact.extra = object.value("extra").toObject().toVariantMap();
    ids.insert(artifact.portId); outputs.artifacts.append(artifact);
  }
  outputs.ok = true;
  return true;
}

AlgorithmRunResult preparedOutputs(const DerivedPreparedSet &prepared) {
  AlgorithmRunResult result; result.ok = !prepared.members.isEmpty();
  for (const auto &member : prepared.members) {
    AlgorithmOutputArtifact artifact; artifact.portId = member.portId;
    artifact.absolutePath = member.staging.absolutePath; artifact.extra = member.extra;
    artifact.parentPortIds = member.parentPortIds; result.artifacts.append(artifact);
  }
  return result;
}
} // namespace paleo::capability::publication_detail
