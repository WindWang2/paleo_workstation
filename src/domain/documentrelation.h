// 层：数据
#pragma once
#include "documentsource.h"
#include <optional>

namespace paleo::document {
enum class ObjectKind { TimeDepth, IntervalTrack, SeismicVolume };
enum class RelationPurpose { WellColumn, Correlation, WellTie, Mapping, FaciesAnalysis };
enum class RelationKind { TimeDepth, StratigraphicTrack };
struct ModelObjectRef {
    SourceRef source;
    ObjectKind kind=ObjectKind::TimeDepth;
    qlonglong id=0;
    QString revision; // Current change token; never promises historical value recovery.
    bool valid() const;
    bool operator==(const ModelObjectRef &) const = default;
};
struct InterpretationContext {
    QString documentId,modelStoreId;
    RelationPurpose purpose=RelationPurpose::WellColumn;
    QString schemeId,schemeRevision,intervalId,subIntervalId;
    bool aggregate=false;
    std::optional<ModelObjectRef> targetVolume;
    bool valid() const;
    QString key() const;
    bool operator==(const InterpretationContext &) const = default;
};
struct RelationUseSpec {
    InterpretationContext context;
    QString wellEntityId;
    qlonglong modelWellId=0;
    RelationKind relation=RelationKind::TimeDepth;
    bool valid() const;
    bool operator==(const RelationUseSpec &) const = default;
};
QJsonObject modelObjectRefJson(const ModelObjectRef &ref);
bool modelObjectRefFromJson(const QJsonObject &json,ModelObjectRef &ref,QString *error=nullptr);
QJsonObject interpretationContextJson(const InterpretationContext &context);
bool interpretationContextFromJson(const QJsonObject &json,InterpretationContext &context,QString *error=nullptr);
QJsonObject relationUseSpecJson(const RelationUseSpec &use);
bool relationUseSpecFromJson(const QJsonObject &json,RelationUseSpec &use,QString *error=nullptr);
bool hasRelationBinding(const SourceBinding &binding);
bool setRelationBinding(SourceBinding &binding,const RelationUseSpec &use,const ModelObjectRef &object,QString *error=nullptr);
bool relationBinding(const SourceBinding &binding,RelationUseSpec &use,ModelObjectRef &object,QString *error=nullptr);
}
Q_DECLARE_METATYPE(paleo::document::ModelObjectRef)
Q_DECLARE_METATYPE(paleo::document::InterpretationContext)
Q_DECLARE_METATYPE(paleo::document::RelationUseSpec)
