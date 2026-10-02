// 层：功能
#pragma once

#include "../algorithms/faultsurface/faultsurface.h"
#include "../domain/faultset.h"
#include "derivedassets.h"

#include <QPair>
#include <QString>
#include <QVector>

class FaultSetStore;

// workflow/faultsurfaceworkflow — 选断层、成面、校验、登记。
// 失败不落 DERIVED 版本。parentVersionIds 只有调用方给出的断层集版本。
namespace paleo::faultsurf {

struct FaultSurfaceRequest {
    paleo::fault::Fault fault;
    SurveyFrame frame;
    QString parentVersionId;
    QString displayName;
    QString sourceUri;
};

struct FaultSurfaceProduceResult {
    bool ok = false;
    QString error;
    SurfaceBuildStatus status = SurfaceBuildStatus::Ok;
    paleo::fault::FaultSurfaceMesh mesh;
    QString versionId;
    QString relativePath;
};

class FaultSurfaceWorkflow {
public:
    FaultSurfaceProduceResult build(const paleo::fault::Fault &fault, const SurveyFrame &frame) const;

    // stage → 写 mesh JSON → commit。校验或成面失败时不 stage。
    FaultSurfaceProduceResult produce(const FaultSurfaceRequest &request, DerivedAssetRegistrar &registrar) const;

    bool saveToStore(FaultSetStore &store, paleo::fault::FaultSet &set, const QString &faultId,
                     const paleo::fault::FaultSurfaceMesh &mesh, QString *error = nullptr) const;

    // 剖面交线，点列 (traceFrac, 纵向样值)，给画布直接画。
    QVector<QPair<double, double>> sectionCut(const paleo::fault::FaultSurfaceMesh &mesh, const SurveyFrame &frame,
                                              const paleo::fault::FaultSectionRef &section, QString *error = nullptr) const;
};

} // namespace paleo::faultsurf
