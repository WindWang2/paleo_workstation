// 层：组装根
#include "wellproviderwiring.h"
#include "workflow/documentsession.h"
#include "workflow/capabilityhost.h"
#include "catalog/datacatalog.h"
#include "qgis/wellattributestore.h"
#include <QPointer>

AiChatToolRunner::FrozenFaciesProvider assembleWellFaciesProvider(DataCatalog *catalog,
    paleo::document::DocumentSession *session,paleo::capability::CapabilityHost *host) {
  if(!catalog||!catalog->isOpen()||!session||!session->isOpen()||!host)return {};
  const QPointer<DataCatalog> guardedCatalog=catalog;
  const QPointer<paleo::document::DocumentSession> guardedSession=session;
  const QPointer<paleo::capability::CapabilityHost> guardedHost=host;
  return [guardedCatalog,guardedSession,guardedHost](const paleo::wellfacies::Request &request,
      const WellFaciesModel &model,AiChatToolRunner::FrozenFaciesInput &out,QString &code) {
    out={};
    if(!guardedCatalog||!guardedCatalog->isOpen()||!guardedSession||!guardedSession->isOpen()||!guardedHost) {
      code="well_provider_unavailable";return false;
    }
    const auto expected=guardedHost->context();
    constexpr auto effects=paleo::host::Read|paleo::host::PrivateStage|paleo::host::ExternalTransfer;
    code=paleo::host::checkContext(expected,expected,effects);
    if(!code.isEmpty())return false;
    if(guardedSession->documentId()!=request.well.documentId||expected.documentId!=request.well.documentId||
       request.context.documentId!=request.well.documentId||request.well.modelStoreId!=QLatin1String("area-model")) {
      code="well_document_mismatch";return false;
    }
    const auto entity=guardedCatalog->entityById(request.well.catalogEntityId);
    if(entity.id.isEmpty()||entity.entityType!=QLatin1String("well")||
       entity.extra.value("areaModelWellId").toLongLong()!=request.well.modelWellId||!request.well.modelAvailable) {
      code="well_mapping_unavailable";return false;
    }
    const auto readiness=paleo::wellfacies::modelReadiness(model,request.modelId,request.modelVersion);
    if(!readiness.isEmpty()){code=readiness;return false;}
    out.job.projectDir=guardedCatalog->projectDir();out.job.request=request;out.job.model=model;
    // Capture actual project leaf classes on the owner thread. Worker reads no live catalog.
    out.job.faciesSchemas=WellAttributeStore::faciesSchemas(guardedCatalog);
    const auto sessionEpoch=guardedSession->epoch();
    out.ownerValidation=[guardedCatalog,guardedSession,guardedHost,request,expected,sessionEpoch]() -> QString {
      if(!guardedCatalog||!guardedCatalog->isOpen()||!guardedSession||!guardedSession->isOpen()||!guardedHost)
        return "document_unavailable";
      auto code=paleo::host::checkContext(guardedHost->context(),expected,effects);
      if(!code.isEmpty())return code;
      if(guardedSession->epoch()!=sessionEpoch||guardedSession->documentId()!=request.well.documentId)return "stale_session";
      const auto live=guardedCatalog->entityById(request.well.catalogEntityId);
      if(live.id.isEmpty()||live.extra.value("areaModelWellId").toLongLong()!=request.well.modelWellId)return "well_mapping_changed";
      // Global model head changes after freezing do not alter the consumed immutable values.
      return {};
    };
    code.clear();return true;
  };
}
