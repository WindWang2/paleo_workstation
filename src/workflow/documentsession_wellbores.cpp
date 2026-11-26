// 层：功能
#include "documentsession.h"
#include "wellboresourceconfirmation.h"
#include "catalog/datacatalog.h"
#include <paleo_data/wellbore_identity.h>
#include <QFileInfo>
#include <QUuid>

namespace paleo::document {
namespace {
bool singleLinkedWell(const DataCatalog &catalog, const QString &assetId, const QString &wellId) {
    bool found = false;
    for (const auto &link : catalog.linksForAsset(assetId)) {
        if (link.entityType != QLatin1String("well")) continue;
        if (link.unresolved || link.entityId != wellId) return false;
        found = true;
    }
    return found;
}
}
SourceCommandResult DocumentSession::confirmWellboreSource(const WellboreSourceConfirmation &input) {
    SourceCommand guard;
    guard.expectedEpoch = input.expectedEpoch;
    guard.expectedRevision = input.expectedRevision;
    guard.kind = SourceCommand::Kind::ConfirmBinding;
    SourceCommandResult result;
    result.operationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    result.revision = m_revision;
    if (!check(guard, true, result)) return result;
    const auto record = source(input.source);
    const auto version = m_catalog->versionById(input.versionId);
    const auto well = m_catalog->storedEntityById(input.wellEntityId);
    const auto head = m_catalog->projectionCheckpoint("area-sources");
    const auto wellId = well.extra.value("areaModelWellId").toLongLong();
    if (record.ref != input.source || record.ref.modelStoreId != "area-model" || !record.fingerprint.valid() ||
        record.versionId != input.versionId || m_catalog->currentVersion(record.assetId).id != input.versionId ||
        version.stage != "RAW" || record.kind != "well_log" || QFileInfo(version.fileName).suffix().compare("las", Qt::CaseInsensitive) ||
        well.entityType != "well" || wellId <= 0 || !well.extra.value("areaModelWellActive", true).toBool() ||
        !singleLinkedWell(*m_catalog, record.assetId, well.id)) {
        result.code = "wellbore_source_unconfirmed";
        result.error = QStringLiteral("请选取当前原始 LAS 版本，先确认其所属井并同步资料索引。");
        return result;
    }
    if (!head.valid() || well.extra.value("areaModelGeneration").toString() != head.generation ||
        version.extra.value("areaModelGeneration").toString() != head.generation) {
        result.code = "model_generation_changed";
        result.error = QStringLiteral("井或来源的模型版本已变化，请刷新后重试。");
        return result;
    }
    data::WellboreDeclaration declaration;
    declaration.wellbore = {{wellId, input.boreKey.toStdString()}, input.displayName.toStdString()};
    declaration.source = data::SourceWellboreKey{input.source.sourceId, "file"};
    result.ok = queuedWrite([&](QString *error) {
        std::string detail;
        if (data::confirmSourceWellbore(record.modelDirectory.toStdString(), head.storeId.toStdString(),
                head.generation.toStdString(), record.fingerprint.stored().toStdString(), declaration, detail)) return true;
        *error = QString::fromStdString(detail);
        return false;
    }, &result.error);
    if (result.ok) {
        result.code = "wellbore_source_confirmed";
        result.versionId = version.id;
        emit sourcesChanged({m_epoch, m_revision, result.operationId, {}, {record.ref.key()}});
    } else if (result.error == QStringLiteral("SourceWellMismatch")) {
        result.code = "wellbore_source_well_mismatch";
        result.error = QObject::tr("无法将这份 LAS 确认到所选井。请核对 LAS 解析出的所属井和资料关联，两者须指向同一口井。");
    } else {
        result.code = result.error.startsWith("WellboreConflict") ? "wellbore_conflict" : "wellbore_source_write_failed";
    }
    return result;
}
}
