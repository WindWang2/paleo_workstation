// 层：数据
#pragma once
#include "domain/documentrelation.h"
#include <QHash>
namespace paleo::document {
// Build on the owner thread from the document/catalog, then pass values to a worker.
struct RelationResolveRequest {
    SourceRecord selected,targetVolume;
    RelationUseSpec use;
    ModelObjectRef object;
    QString catalogDocumentId,wellEntityId;
    qlonglong mappedModelWellId=0;
    // DocumentSession stamps a current unique primary choice. Data consumers
    // reject a request after any catalog edit; metadata-only validation may omit it.
    QString selectedBindingId;
    quint64 catalogRevision=0;
};
struct RelationResolution {
    bool ok=false;
    QString code,error;
    ModelObjectRef object;
};
struct RelationInput {
    enum class State { Unselected, Selected, Invalid };
    State state=State::Unselected;
    RelationResolveRequest request;
    QString error;
};
struct InterpretationSelection {
    std::optional<InterpretationContext> context;
    SourceRecord volume;
    QString error;
};
struct InterpretationReadPlan {
    InterpretationSelection selection;
    QHash<QString,RelationInput> timeDepth;
};
QString validateInterpretationSelection(const QString &modelDirectory,const InterpretationSelection &selection,
                                        QString *timeReference=nullptr);
// Opens read-only and point-reads metadata. Selecting a relation never confirms units/datums.
RelationResolution resolveDocumentRelation(const RelationResolveRequest &request);
}
