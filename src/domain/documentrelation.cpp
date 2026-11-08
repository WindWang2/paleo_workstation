// 层：数据
#include "documentrelation.h"
#include <QCryptographicHash>
#include <QJsonDocument>

namespace paleo::document {
namespace {
bool fail(QString *error,const QString &message) {if(error)*error=message;return false;}
QString objectName(ObjectKind kind) {
    switch(kind) {case ObjectKind::TimeDepth:return "time_depth";case ObjectKind::IntervalTrack:return "intervals";case ObjectKind::SeismicVolume:return "volumes";}
    return {};
}
QString purposeName(RelationPurpose purpose) {
    switch(purpose) {case RelationPurpose::WellColumn:return "well-column";case RelationPurpose::Correlation:return "correlation";
    case RelationPurpose::WellTie:return "well-tie";case RelationPurpose::Mapping:return "mapping";case RelationPurpose::FaciesAnalysis:return "facies-analysis";}
    return {};
}
QString relationName(RelationKind kind) {return kind==RelationKind::TimeDepth?"time_depth":kind==RelationKind::StratigraphicTrack?"stratigraphy":QString();}
bool textFields(const QJsonObject &json,const QStringList &fields) {
    for(const auto &field:fields)if(!json.value(field).isString())return false;
    return true;
}
bool matches(const RelationUseSpec &use,const ModelObjectRef &object) {
    return use.valid() && object.valid() && object.source.documentId==use.context.documentId &&
        object.source.modelStoreId==use.context.modelStoreId &&
        object.kind==(use.relation==RelationKind::TimeDepth?ObjectKind::TimeDepth:ObjectKind::IntervalTrack);
}
}
bool ModelObjectRef::valid() const {return source.valid() && source.sourceId>0 && !objectName(kind).isEmpty() && id>0 && !revision.isEmpty() && revision.size()<=256;}
bool InterpretationContext::valid() const {
    if(documentId.isEmpty() || modelStoreId.isEmpty() || purposeName(purpose).isEmpty() || schemeId.isEmpty()!=schemeRevision.isEmpty())return false;
    if(schemeId.isEmpty() && (!intervalId.isEmpty() || !subIntervalId.isEmpty() || aggregate))return false;
    if(!subIntervalId.isEmpty() && (intervalId.isEmpty() || aggregate))return false;
    if(!intervalId.isEmpty() && subIntervalId.isEmpty() && !aggregate)return false;
    if(aggregate && (intervalId.isEmpty() || !subIntervalId.isEmpty()))return false;
    return !targetVolume || (targetVolume->valid() && targetVolume->kind==ObjectKind::SeismicVolume &&
        targetVolume->source.documentId==documentId && targetVolume->source.modelStoreId==modelStoreId);
}
QString InterpretationContext::key() const {
    if(!valid())return {};
    return "interpretation:"+QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(interpretationContextJson(*this)).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
bool RelationUseSpec::valid() const {return context.valid() && !wellEntityId.isEmpty() && modelWellId>0 && !relationName(relation).isEmpty();}
QJsonObject modelObjectRefJson(const ModelObjectRef &ref) {
    return {{"source",sourceRefJson(ref.source)},{"kind",objectName(ref.kind)},{"id",QString::number(ref.id)},{"revision",ref.revision}};
}
bool modelObjectRefFromJson(const QJsonObject &json,ModelObjectRef &ref,QString *error) {
    if(error)error->clear();
    ref={};bool numeric=false;
    if(!textFields(json,{"kind","id","revision"}) || !sourceRefFromJson(json.value("source").toObject(),ref.source))return fail(error,QStringLiteral("对象来源或格式无效"));
    const auto kind=json.value("kind").toString();
    if(kind=="time_depth")ref.kind=ObjectKind::TimeDepth;else if(kind=="intervals")ref.kind=ObjectKind::IntervalTrack;
    else if(kind=="volumes")ref.kind=ObjectKind::SeismicVolume;else return fail(error,QStringLiteral("对象类别不受支持"));
    ref.id=json.value("id").toString().toLongLong(&numeric);ref.revision=json.value("revision").toString();
    return (numeric && ref.valid()) || fail(error,QStringLiteral("对象编号或修订无效"));
}
QJsonObject interpretationContextJson(const InterpretationContext &context) {
    QJsonObject out{{"documentId",context.documentId},{"modelStoreId",context.modelStoreId},{"purpose",purposeName(context.purpose)},
        {"schemeId",context.schemeId},{"schemeRevision",context.schemeRevision},{"intervalId",context.intervalId},{"subIntervalId",context.subIntervalId},{"aggregate",context.aggregate}};
    if(context.targetVolume)out.insert("targetVolume",modelObjectRefJson(*context.targetVolume));return out;
}
bool interpretationContextFromJson(const QJsonObject &json,InterpretationContext &context,QString *error) {
    if(error)error->clear();
    context={};
    if(!textFields(json,{"documentId","modelStoreId","purpose","schemeId","schemeRevision","intervalId","subIntervalId"}) || !json.value("aggregate").isBool())return fail(error,QStringLiteral("解释上下文字段无效"));
    context.documentId=json.value("documentId").toString();context.modelStoreId=json.value("modelStoreId").toString();
    const auto purpose=json.value("purpose").toString();
    if(purpose=="well-column")context.purpose=RelationPurpose::WellColumn;else if(purpose=="correlation")context.purpose=RelationPurpose::Correlation;
    else if(purpose=="well-tie")context.purpose=RelationPurpose::WellTie;else if(purpose=="mapping")context.purpose=RelationPurpose::Mapping;
    else if(purpose=="facies-analysis")context.purpose=RelationPurpose::FaciesAnalysis;else return fail(error,QStringLiteral("解释用途不受支持"));
    context.schemeId=json.value("schemeId").toString();context.schemeRevision=json.value("schemeRevision").toString();
    context.intervalId=json.value("intervalId").toString();context.subIntervalId=json.value("subIntervalId").toString();context.aggregate=json.value("aggregate").toBool();
    if(json.contains("targetVolume")) {ModelObjectRef ref;if(!modelObjectRefFromJson(json.value("targetVolume").toObject(),ref,error))return false;context.targetVolume=ref;}
    return context.valid() || fail(error,QStringLiteral("解释上下文缺少固定修订或对象归属不一致"));
}
QJsonObject relationUseSpecJson(const RelationUseSpec &use) {
    return {{"context",interpretationContextJson(use.context)},{"wellEntityId",use.wellEntityId},{"modelWellId",QString::number(use.modelWellId)},{"relation",relationName(use.relation)}};
}
bool relationUseSpecFromJson(const QJsonObject &json,RelationUseSpec &use,QString *error) {
    if(error)error->clear();
    use={};bool numeric=false;
    if(!textFields(json,{"wellEntityId","modelWellId","relation"}) || !interpretationContextFromJson(json.value("context").toObject(),use.context,error))return fail(error,QStringLiteral("关系用途格式无效"));
    use.wellEntityId=json.value("wellEntityId").toString();use.modelWellId=json.value("modelWellId").toString().toLongLong(&numeric);
    const auto relation=json.value("relation").toString();
    if(relation=="time_depth")use.relation=RelationKind::TimeDepth;else if(relation=="stratigraphy")use.relation=RelationKind::StratigraphicTrack;else return fail(error,QStringLiteral("关系类别不受支持"));
    return (numeric && use.valid()) || fail(error,QStringLiteral("关系用途缺少井身份"));
}
bool hasRelationBinding(const SourceBinding &binding) {
    // Only the reserved format marker opts into typed interpretation. Legacy
    // locators may already use arbitrary scopes and common field names.
    // Future markers still enter the strict parser and must not fall back.
    return binding.locator.fields.value("key").toString().startsWith("document-relation-v");
}
bool setRelationBinding(SourceBinding &binding,const RelationUseSpec &use,const ModelObjectRef &object,QString *error) {
    if(error)error->clear();
    if(!matches(use,object))return fail(error,QStringLiteral("对象与关系用途不一致"));
    binding.resourceKind="well";binding.resourceId=use.wellEntityId;binding.role=relationName(use.relation);binding.scope=use.context.key();binding.source=object.source;
    binding.locator={LocatorKind::RecordGroup,{{"key","document-relation-v1"},{"schema",1},{"use",relationUseSpecJson(use).toVariantMap()},{"objectRef",modelObjectRefJson(object).toVariantMap()}}};return true;
}
bool relationBinding(const SourceBinding &binding,RelationUseSpec &use,ModelObjectRef &object,QString *error) {
    if(error)error->clear();
    use={};object={};const auto &fields=binding.locator.fields;
    const auto schemaType=fields.value("schema").metaType().id();
    const bool numericSchema=schemaType==QMetaType::Int || schemaType==QMetaType::LongLong;
    if(binding.locator.kind!=LocatorKind::RecordGroup || fields.value("key").toString()!="document-relation-v1" || fields.size()!=4 ||
       !numericSchema || fields.value("schema").toLongLong()!=1 ||
       fields.value("use").metaType().id()!=QMetaType::QVariantMap || fields.value("objectRef").metaType().id()!=QMetaType::QVariantMap)
        return fail(error,QStringLiteral("旧来源用途或类型化关系格式无效，需明确重新确认"));
    if(!relationUseSpecFromJson(QJsonObject::fromVariantMap(fields.value("use").toMap()),use,error) ||
       !modelObjectRefFromJson(QJsonObject::fromVariantMap(fields.value("objectRef").toMap()),object,error))return false;
    return (matches(use,object) && binding.resourceKind=="well" && binding.resourceId==use.wellEntityId && binding.role==relationName(use.relation) &&
        binding.source==object.source && binding.scope==use.context.key()) || fail(error,QStringLiteral("绑定与类型化用途或对象身份不一致"));
}

QString timeDepthSelectionKey(const RelationUseSpec &use)
{
    SourceBinding slot;
    slot.resourceKind = QStringLiteral("well");
    slot.resourceId = use.wellEntityId;
    slot.role = QStringLiteral("time_depth");
    slot.scope = use.context.key();
    return slot.slotKey();
}

QString TimeDepthDependency::slotKey() const
{
    return timeDepthSelectionKey(use);
}

bool TimeDepthDependency::valid() const
{
    return !assetId.isEmpty() && use.context.modelStoreId == QLatin1String("area-model") &&
        use.relation == RelationKind::TimeDepth && matches(use, object);
}

QVariantMap TimeDepthDependency::toVariantMap() const
{
    return {{"format", QStringLiteral("time-depth-follow-selection-v1")}, {"assetId", assetId},
        {"use", relationUseSpecJson(use).toVariantMap()}, {"objectRef", modelObjectRefJson(object).toVariantMap()}};
}

std::optional<TimeDepthDependency> TimeDepthDependency::fromVariant(const QVariant &value)
{
    if (value.metaType().id() != QMetaType::QVariantMap)
        return std::nullopt;
    const auto json = QJsonObject::fromVariantMap(value.toMap());
    TimeDepthDependency dependency;
    dependency.assetId = json.value("assetId").toString();
    if (json.size() != 4 || json.value("format").toString() != QLatin1String("time-depth-follow-selection-v1") ||
        !relationUseSpecFromJson(json.value("use").toObject(), dependency.use) ||
        !modelObjectRefFromJson(json.value("objectRef").toObject(), dependency.object) || !dependency.valid())
        return std::nullopt;
    return dependency;
}

}
