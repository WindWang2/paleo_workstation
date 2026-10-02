// 层：功能
#include "faultsurfaceworkflow.h"

#include "../metadata/faultsetstore.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace paleo::faultsurf {
namespace {

void setError(QString *error, const QString &text)
{
    if (error)
        *error = text;
}

} // namespace

FaultSurfaceProduceResult FaultSurfaceWorkflow::build(const paleo::fault::Fault &fault,
                                                      const SurveyFrame &frame) const
{
    FaultSurfaceProduceResult result;
    const SurfaceBuildResult built = buildFaultSurface(fault, frame);
    result.status = built.status;
    result.mesh = built.mesh;
    result.error = built.message;
    if (!built.ok())
        return result;
    const MeshTopology topo = validateMeshTopology(built.mesh);
    if (!topo.ok) {
        result.status = SurfaceBuildStatus::DegenerateMesh;
        result.error = topo.message;
        result.mesh = {};
        return result;
    }
    result.ok = true;
    return result;
}

FaultSurfaceProduceResult FaultSurfaceWorkflow::produce(const FaultSurfaceRequest &request,
                                                       DerivedAssetRegistrar &registrar) const
{
    FaultSurfaceProduceResult result = build(request.fault, request.frame);
    if (!result.ok)
        return result;
    if (request.parentVersionId.isEmpty()) {
        result.ok = false;
        result.error = QStringLiteral("缺少源 FaultSet 版本，拒绝登记断面");
        result.mesh = {};
        return result;
    }
    if (!registrar.isBound()) {
        result.ok = false;
        result.error = QStringLiteral("派生产物登记未绑定 catalog");
        result.mesh = {};
        return result;
    }

    const QString name = request.displayName.isEmpty()
                             ? (request.fault.name.isEmpty() ? request.fault.id : request.fault.name)
                             : request.displayName;
    QString stageErr;
    const DerivedStaging staging =
        registrar.stage(QStringLiteral("fault_surface"), name, QStringLiteral("fault-surface.json"), &stageErr);
    if (!staging.isValid()) {
        result.ok = false;
        result.error = stageErr;
        result.mesh = {};
        return result;
    }

    QFile file(staging.absolutePath);
    if (!file.open(QIODevice::WriteOnly)) {
        result.ok = false;
        result.error = QStringLiteral("无法写入断面 %1").arg(staging.absolutePath);
        result.mesh = {};
        return result;
    }
    const QByteArray body =
        QJsonDocument(QJsonObject::fromVariantMap(result.mesh.toMap())).toJson(QJsonDocument::Compact);
    if (file.write(body) != body.size()) {
        result.ok = false;
        result.error = QStringLiteral("断面写入不完整");
        result.mesh = {};
        return result;
    }
    file.close();

    QVariantMap extra;
    extra.insert(QStringLiteral("faultId"), request.fault.id);
    extra.insert(QStringLiteral("vertices"), result.mesh.vertices.size());
    extra.insert(QStringLiteral("triangles"), result.mesh.triangles.size());
    const QString source = request.sourceUri.isEmpty()
                               ? QStringLiteral("fault-surface:%1").arg(request.fault.id)
                               : request.sourceUri;
    QString commitErr;
    if (!registrar.commit(staging, {request.parentVersionId}, source, extra, &commitErr)) {
        result.ok = false;
        result.error = commitErr;
        result.versionId.clear();
        result.mesh = {};
        return result;
    }
    result.versionId = staging.versionId;
    result.relativePath = staging.relativePath;
    return result;
}

bool FaultSurfaceWorkflow::saveToStore(FaultSetStore &store, paleo::fault::FaultSet &set, const QString &faultId,
                                       const paleo::fault::FaultSurfaceMesh &mesh, QString *error) const
{
    if (!set.setSurface(faultId, mesh)) {
        setError(error, QStringLiteral("断层 %1 不在 FaultSet 中").arg(faultId));
        return false;
    }
    return store.save(set, error);
}

QVector<QPair<double, double>> FaultSurfaceWorkflow::sectionCut(const paleo::fault::FaultSurfaceMesh &mesh,
                                                                const SurveyFrame &frame,
                                                                const paleo::fault::FaultSectionRef &section,
                                                                QString *error) const
{
    const SectionCut cut = intersectSurfaceWithSection(mesh, frame, section);
    if (!cut.ok()) {
        setError(error, cut.message);
        return {};
    }
    QVector<QPair<double, double>> points;
    points.reserve(cut.hits.size());
    for (const SectionHit &hit : cut.hits)
        points.append({hit.traceFrac, hit.sample});
    return points;
}

} // namespace paleo::faultsurf
