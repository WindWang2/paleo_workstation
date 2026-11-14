// 层：功能
#include "tileoperationservice.h"
#include "capabilityhost.h"
#include "capabilityrecovery.h"
#include "ai/onnxpredictionservice.h"
#include "services/paleotaskservice.h"
#include <QJsonDocument>
#include <QFileInfo>
#include <QThread>

namespace paleo {
namespace {
capability::OperationWorker tileWorker(const QPointer<PaleoOnnxService> &onnx,const QString &location,const QString &bindingError) {
  return [onnx,location,bindingError](const capability::OperationWorkContext &work) {
    capability::OperationWorkResult result;FrozenTilePlan plan;QString error;
    if(!bindingError.isEmpty()){result.code=bindingError;return result;}
    if(!onnx || !FrozenTilePlan::fromJson(work.frozenPlan,plan,&error)) {
      result.code=onnx?error:QStringLiteral("ort_unavailable");return result;
    }
    plan.raster.path=location;
    result.outputs=computeTilePlan(onnx,plan,work.privateDir,result.summary,work.cancelled);
    result.ok=result.outputs.ok;result.code=result.outputs.error;return result;
  };
}
capability::OperationInputValidator tileInputValidator(const QString &location,const QString &bindingError) {
  return [location,bindingError](const capability::OperationWorkContext &work) {
    if(!bindingError.isEmpty())return bindingError;
    FrozenTilePlan plan;QString error;
    if(!FrozenTilePlan::fromJson(work.frozenPlan,plan,&error))return error;
    plan.raster.path=location;
    FrozenRasterReader reader(plan.raster);
    if(!reader.open(work.cancelled,&error))return error;
    return QString();
  };
}
}
TileOperationService::TileOperationService(QObject *parent):QObject(parent) {}
void TileOperationService::configure(capability::CapabilityHost *host,PaleoTaskService *tasks,PaleoOnnxService *onnx,
                                     SourceProvider source,PresentationProvider presentation) {
  const bool changed=m_host!=host;m_host=host;m_tasks=tasks;m_onnx=onnx;
  m_source=std::move(source);m_presentation=std::move(presentation);
  if(changed && host) {
    const QPointer<TileOperationService> guard(this);
    host->setWorkerRestorer([guard](const capability::OperationLog::Record &record,QString *code) -> capability::OperationWorker {
      if(record.capabilityId!=QLatin1String("tile.classify") || record.contractVersion!=2)return {};
      const auto reject=[&](const QString &value)->capability::OperationWorker {if(code)*code=value;return {};};
      if(!guard)return reject("tile_provider_unavailable");
#if !PALEO_HAVE_ORT
      return reject("ort_unavailable");
#else
      if(!guard->m_onnx)return reject("ort_provider_unavailable");
      FrozenTilePlan plan;QString invalid;
      if(!FrozenTilePlan::fromJson(record.frozenRequest.value("plan").toObject(),plan,&invalid))return reject(invalid);
      if(!guard->m_onnx->availableModels().contains(plan.model))return reject("model_unavailable");
      FrozenRasterInput input;QString error;const auto version=record.frozenRequest.value("plan").toObject().value("raster").toObject().value("versionId").toString();
      if(!guard->m_source || !guard->m_source(version,input,&error))error=error.isEmpty()?QStringLiteral("tile_source_provider_unavailable"):error;
      if(!error.isEmpty())return reject(error);
      return tileWorker(guard->m_onnx,input.path,error);
#endif
    });
    host->setInputValidatorRestorer([guard](const capability::OperationLog::Record &record,QString *code) -> capability::OperationInputValidator {
      if(record.capabilityId!=QLatin1String("tile.classify") || record.contractVersion!=2)return {};
      if(!guard){if(code)*code="tile_provider_unavailable";return {};}
      FrozenRasterInput input;QString error;const auto version=record.frozenRequest.value("plan").toObject().value("raster").toObject().value("versionId").toString();
      if(!guard->m_source || !guard->m_source(version,input,&error))error=error.isEmpty()?QStringLiteral("tile_source_provider_unavailable"):error;
      if(!error.isEmpty()){if(code)*code=error;return {};}
      return tileInputValidator(input.path,error);
    });
    host->setFrozenValidator([guard](const capability::OperationLog::Record &record) -> QString {
      if(record.capabilityId!=QLatin1String("tile.classify"))return {};
      if(!guard || !guard->m_source)return "tile_source_provider_unavailable";
      FrozenTilePlan plan;FrozenRasterInput current;QString error;
      if(!FrozenTilePlan::fromJson(record.frozenRequest.value("plan").toObject(),plan,&error))return error;
      if(!guard->m_source(plan.raster.versionId,current,&error))return error;
      QJsonObject declared;
      if(!current.catalogFeatureContract.isEmpty() && !normalizeRasterFeatureContract(current.catalogFeatureContract,declared))
        return "source_feature_contract_invalid";
      if((!plan.raster.catalogFeatureContract.isEmpty() && declared!=plan.raster.catalogFeatureContract) ||
         (!declared.isEmpty() && declared!=plan.raster.featureContract))return "source_feature_contract_changed";
      if(current.versionId!=plan.raster.versionId || current.sha256!=plan.raster.sha256)return "raster_catalog_binding_changed";
      return {};
    });
  }
}
QString TileOperationService::readiness(const QJsonObject &args,bool listing) const {
  if(QThread::currentThread()!=thread())return "owner_thread_required";
  if(!m_host || !m_host->recovery())return "recovery_service_unavailable";
  if(!m_tasks)return "task_service_unavailable";
  const auto phase=args.value("publish_intent").toString();
  if(listing || (phase!=QLatin1String("prepare") && phase!=QLatin1String("compute-only")))return {};
#if !PALEO_HAVE_ORT
  return "ort_unavailable";
#else
  if(!m_source)return "tile_source_provider_unavailable";
  if(!m_onnx)return "ort_provider_unavailable";
  if(m_onnx->availableModels().isEmpty())return "model_unavailable";
  return {};
#endif
}
capability::FrozenOperation TileOperationService::operation(const QJsonObject &intent,const QString &scope) const {
  capability::FrozenOperation op;op.capabilityId="tile.classify";op.contractVersion=2;op.implementationVersion="2";
  op.callerScope=scope;op.clientRequestId=intent.value("client_request_id").toString();
  op.intent={{"capabilityId",op.capabilityId},{"contractVersion",2},{"arguments",intent}};
  op.phase=intent.value("publish_intent").toString();op.sourceOperationId=intent.value("source_operation_id").toString();
  if(op.phase==QLatin1String("prepare")) {
    op.outputSpec=AlgorithmSpecRegistry::specAtVersion("onnx:tile_classification",2);
    op.outputPorts={"OUTPUT","MASKED","CONFIDENCE"};
  }
  // The owner restorer resolves current file locations for the same pinned values.
  return op;
}
PaleoTask *TileOperationService::start(const QJsonObject &intent,const QString &scope,QObject *subscriber,
    std::function<void(capability::ExecuteResult)> finished,QString *error) {
  const auto reject=[&](const QString &code)->PaleoTask * {if(error)*error=code;return nullptr;};
  if(QThread::currentThread()!=thread())return reject("owner_thread_required");
  if(!m_host || !m_host->recovery() || !m_tasks || !subscriber)return reject("host_unavailable");
  const auto op=operation(intent,scope);
  if(scope.isEmpty() || op.clientRequestId.isEmpty())return reject("client_request_id_required");
  // The original intent wins before any live source/model/provider lookup.
  const auto prior=m_host->recovery()->lookup(scope,op.clientRequestId);
  if(prior.code==QLatin1String("found")) {
    if(prior.record.requestHash!=capability::operationDigest(op.intent))return reject("idempotency_conflict");
    return m_host->recovery()->start(scope,prior.record.operationId,subscriber,"Tile operation",std::move(finished),false,error);
  }
  if(prior.code!=QLatin1String("not_found"))return reject(prior.code);
  if(op.phase==QLatin1String("prepare"))return prepare(intent,scope,subscriber,std::move(finished),error);
  if((op.phase!=QLatin1String("compute-only") && op.phase!=QLatin1String("publish")) || op.sourceOperationId.isEmpty())
    return reject("source_operation_required");
  if(intent.contains("model") || intent.contains("source_version_id") || intent.contains("parameters"))
    return reject("inherited_tile_inputs_are_immutable");
  const auto unavailable=readiness(intent,false);if(!unavailable.isEmpty())return reject(unavailable);
  const auto accepted=m_host->recovery()->accept(op);
  if(!accepted.ok)return reject(accepted.code);
  return m_host->recovery()->start(scope,accepted.record.operationId,subscriber,"Tile operation",std::move(finished),false,error);
}
}
