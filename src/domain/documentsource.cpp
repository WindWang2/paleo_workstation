// 层：数据
#include "documentsource.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace paleo::document {
namespace {
QString tupleKey(const QStringList &parts) {
    QJsonArray array; for (const auto &part : parts) array.append(part);
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(array).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
bool fail(QString *error, const QString &message) { if(error) *error=message; return false; }
}
SourceFingerprint SourceFingerprint::fromStored(const QString &stored) {
    SourceFingerprint result;
    result.kind = stored.startsWith("quick:") ? FingerprintKind::SegyQuickV1 : FingerprintKind::Sha256;
    result.value = stored.startsWith("quick:") ? stored.mid(6).toLower() : stored.toLower();
    return result.valid() ? result : SourceFingerprint{};
}
bool SourceFingerprint::valid() const {
    static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
    return (kind==FingerprintKind::Sha256 || kind==FingerprintKind::SegyQuickV1) && digest.match(value).hasMatch();
}
QString SourceFingerprint::stored() const {
    return valid() ? (kind==FingerprintKind::SegyQuickV1 ? QStringLiteral("quick:")+value : value) : QString();
}
bool SourceRef::valid() const {
    return !documentId.isEmpty() && !modelStoreId.isEmpty() && !fileRevisionId.isEmpty() &&
        (sourceId>0 || (sourceId==0 && modelStoreId.startsWith("catalog:")));
}
QString SourceRef::key() const { return tupleKey({documentId,modelStoreId,QString::number(sourceId),fileRevisionId}); }
bool SourceLocator::valid() const {
    if(kind==LocatorKind::File) return fields.isEmpty();
    for(const auto &key:QStringList{"rowFirst","columnFirst","rowCount","columnCount","sampleCount"})
        if(fields.contains(key) && fields.value(key).toLongLong()<=0) return false;
    if(fields.contains("sampleFirst") && fields.value("sampleFirst").toLongLong()<0) return false;
    const auto text=[this](const char *key){return !fields.value(QLatin1String(key)).toString().trimmed().isEmpty();};
    switch(kind) {
    case LocatorKind::Worksheet: return text("sheet");
    case LocatorKind::RecordGroup: return text("key");
    case LocatorKind::SegyWindow: return text("layout");
    case LocatorKind::ArchiveMember: return text("member");
    case LocatorKind::PackageRelationship: return text("part") && text("relationshipId");
    case LocatorKind::ImageCalibration: return text("calibrationRevision");
    default: return false;
    }
}
QString SourceBinding::resourceKey() const { return tupleKey({resourceKind,resourceId}); }
QString SourceBinding::slotKey() const { return tupleKey({resourceKind,resourceId,role,scope}); }
bool SourceBinding::conflictingIdentities() const {
    QSet<QString> values;
    for(const auto &claim:identityClaims) if(!claim.value.trimmed().isEmpty()) values.insert(claim.value.trimmed().toUpper());
    return values.size()>1;
}
bool SourceBinding::valid(QString *error) const {
    if(error) error->clear();
    if(id.isEmpty() || resourceKind.isEmpty() || resourceId.isEmpty() || role.isEmpty() || !source.valid() || !locator.valid())
        return fail(error,QStringLiteral("来源关联缺少身份、角色或内部定位"));
    if(int(decision)<0 || int(decision)>int(BindingDecision::Rejected)) return fail(error,QStringLiteral("来源关联状态无效"));
    if(primary && decision!=BindingDecision::Confirmed) return fail(error,QStringLiteral("只有已确认的来源关联可设为主来源"));
    if(primary && scope.isEmpty() && locator.kind!=LocatorKind::File)
        return fail(error,QStringLiteral("文件内部来源的主选需要指定用途范围"));
    if(decision==BindingDecision::Confirmed && conflictingIdentities() && resolution.trimmed().isEmpty())
        return fail(error,QStringLiteral("身份声明冲突，需明确记录确认理由"));
    if(QJsonDocument(sourceBindingJson(*this)).toJson(QJsonDocument::Compact).size()>65536)
        return fail(error,QStringLiteral("来源关联说明超过64 KiB预算"));
    return true;
}
QJsonObject sourceRefJson(const SourceRef &ref) {
    return {{"documentId",ref.documentId},{"modelStoreId",ref.modelStoreId},{"sourceId",QString::number(ref.sourceId)},{"fileRevisionId",ref.fileRevisionId}};
}
bool sourceRefFromJson(const QJsonObject &json,SourceRef &ref) {
    bool numeric=false;const auto id=json["sourceId"].toString().toLongLong(&numeric);
    ref={json["documentId"].toString(),json["modelStoreId"].toString(),id,json["fileRevisionId"].toString()};
    return numeric && ref.valid();
}
QJsonObject sourceBindingJson(const SourceBinding &binding) {
    QJsonArray claims;for(const auto &claim:binding.identityClaims) claims.append(QJsonObject{{"origin",claim.origin},{"value",claim.value}});
    return {{"schema",1},{"id",binding.id},{"resourceKind",binding.resourceKind},{"resourceId",binding.resourceId},
        {"role",binding.role},{"scope",binding.scope},{"source",sourceRefJson(binding.source)},
        {"locatorKind",int(binding.locator.kind)},{"locator",QJsonObject::fromVariantMap(binding.locator.fields)},
        {"decision",int(binding.decision)},{"claims",claims},{"evidence",binding.evidence},{"resolution",binding.resolution},
        {"primary",binding.primary},{"revision",QString::number(binding.revision)}};
}
bool sourceBindingFromJson(const QJsonObject &json,SourceBinding &binding,QString *error) {
    binding={};
    if(json["schema"].toInt()!=1 || !sourceRefFromJson(json["source"].toObject(),binding.source))
        return fail(error,QStringLiteral("不支持的来源关联格式"));
    binding.id=json["id"].toString();binding.resourceKind=json["resourceKind"].toString();binding.resourceId=json["resourceId"].toString();
    binding.role=json["role"].toString();binding.scope=json["scope"].toString();binding.evidence=json["evidence"].toString();
    binding.resolution=json["resolution"].toString();binding.primary=json["primary"].toBool();binding.revision=json["revision"].toString().toULongLong();
    binding.decision=BindingDecision(json["decision"].toInt(-1));binding.locator.kind=LocatorKind(json["locatorKind"].toInt(-1));
    binding.locator.fields=json["locator"].toObject().toVariantMap();
    for(const auto &entry:json["claims"].toArray()) {const auto claim=entry.toObject();binding.identityClaims.append({claim["origin"].toString(),claim["value"].toString()});}
    return binding.valid(error);
}
}
