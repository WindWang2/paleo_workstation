// 层：测试壳
#pragma once
#include "../fixtures/documentsourcefixture.h"
#include "app/aiwiring.h"
#include "app/capabilityhostwiring.h"
#include "ai/onnxfixture.h"
#include "ai/onnxpredictionservice.h"
#include "algorithms/projectgeotiff.h"
#include "linkage/selectioncontext.h"
#include "metadata/projectlock.h"
#include "qgis/qgisprojectservice.h"
#include "qgis/qgislayerservice.h"
#include "workflow/aichatcontroller.h"
#include "workflow/capabilityhost.h"
#include <QElapsedTimer>
#include <QJsonDocument>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

namespace tile_agent_fixture {
inline bool raster(const QString &path) {
  GDALAllRegister();auto ds=ProjectGeoTiff::create(path,4,4,GDT_Float32);if(!ds)return false;
  double gt[6]={100,2,0,200,0,-2};GDALSetGeoTransform(ds,gt);
  OGRSpatialReference crs;crs.SetFromUserInput("EPSG:32650");char *wkt=nullptr;crs.exportToWkt(&wkt);
  GDALSetProjection(ds,wkt);CPLFree(wkt);auto band=GDALGetRasterBand(ds,1);
  GDALSetRasterUnitType(band,"dimensionless");GDALSetMetadataItem(band,"PALEO_FEATURE_CONTRACT",
    R"({"version":1,"kind":"scalar","unit":"dimensionless","domain":"none","preprocessing":"raw_float32_v1"})",nullptr);
  float values[16];for(int i=0;i<16;++i)values[i]=float(i-8);
  const bool ok=GDALRasterIO(band,GF_Write,0,0,4,4,values,4,4,GDT_Float32,0,0)==CE_None;
  return GDALClose(ds)==CE_None && ok;
}
struct Fixture {
  document_fixture::Fixture data;
  ProjectDirLock lock{data.project};
  LayerManifest manifest{data.meta};
  QgisProjectService project;
  QgisLayerService layers{&project,&manifest};
  SelectionContext selection;
  paleo::capability::CapabilityHost host;
  PaleoOnnxService onnx;
  AiAssistWorkflow assist{&layers};
  AiChatController chat;
  QString sourceId,error;
  bool init() {
    if(!data.init() || !raster(data.temp.filePath("source.tif")))return false;
    QFile source(data.temp.filePath("source.tif"));if(!source.open(QIODevice::ReadOnly))return false;
    sourceId=data.external("input.tif",source.readAll());
    if(sourceId.isEmpty() || !data.bind() || !lock.tryLock(&error) || !manifest.open(&error))return false;
    selection.setActiveHorizon("T1");configureCapabilityCallers(&host);host.setTaskService(&data.tasks);
    host.setBindingProvider([this]{return captureCapabilityBinding(&data.session,&data.catalog,&lock,&selection,chat.toolRunner());});
    data.session.setHostContextProvider([this]{return host.context();});
    if(!OnnxFixtureWriter::writeSeg(data.temp.filePath("model.onnx"),{1,-1},{0,0}))return false;
    onnx.setModelRoot(data.temp.path());ModelRegistryScan scan;scan.manifestFound=true;
    ModelRegistryEntry entry;entry.name="model";entry.file="model.onnx";entry.absolutePath=data.temp.filePath(entry.file);
    entry.sha256Pinned=PaleoOnnxService::sha256OfFile(entry.absolutePath);entry.outputSemantics="logits";entry.classes=2;
    entry.featureContractVersion=1;entry.features=QJsonArray{QJsonObject{{"kind","scalar"},{"unit","dimensionless"},
      {"domain","none"},{"preprocessing","raw_float32_v1"}}};scan.entries.append(entry);onnx.setModelRegistry(scan);
    assist.setOnnxService(&onnx);assist.setCatalog(&data.catalog,data.project);assist.setTaskService(&data.tasks);
    chat.toolRunner()->setTaskService(&data.tasks);rebind(true);return true;
  }
  void rebind(bool model) {
    assist.setOnnxService(model?&onnx:nullptr);
    bindChatToolRunner(&chat,&assist,&layers,&data.catalog,data.project,data.session.documentId(),&host,&data.session);
  }
  QJsonObject call(const QString &name,const QJsonObject &args) {
    QSignalSpy done(chat.toolRunner(),&AiChatToolRunner::toolFinished);
    chat.toolRunner()->run({QUuid::createUuid().toString(),name,QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))});
    QElapsedTimer timer;timer.start();
    while(done.isEmpty() && timer.elapsed()<15000)QCoreApplication::processEvents(QEventLoop::AllEvents,20);
    if(done.isEmpty())return {{"ok",false},{"code","test_timeout"}};
    auto out=QJsonDocument::fromJson(done[0][2].toString().toUtf8()).object();out.insert("toolOk",done[0][1].toBool());return out;
  }
  QJsonObject phase(const QString &key,const QString &phase,const QString &source={}) {
    QJsonObject args{{"client_request_id",key},{"publish_intent",phase}};
    if(source.isEmpty()) {args.insert("source_version_id",sourceId);args.insert("model","model");
      args.insert("parameters",QJsonObject{{"TILE_ROWS",2},{"TILE_COLUMNS",2},{"HALO",0}});}
    else args.insert("source_operation_id",source);
    return call("paleo.tile_classification_v2",args);
  }
  QJsonObject get(const QString &id) {return call("paleo.operation_get",{{"operation_id",id}});}
  QgsMapLayer *previousLayer() {
    const auto path=data.temp.filePath("old-display.tif");if(!raster(path))return nullptr;
    LayerDeclaration declaration;declaration.layerId="aifacies.T1.model";declaration.horizon="T1";
    declaration.type="raster";declaration.source=path;declaration.title="Previous result";declaration.group="03_Predict";
    if(!layers.declare(declaration,&error))return nullptr;
    return layers.instantiate(declaration.layerId,&error);
  }
  QString compute(const QString &key) {
    const auto prepared=phase(key+"-prepare","prepare");
    if(!prepared.value("toolOk").toBool()){error=QString::fromUtf8(QJsonDocument(prepared).toJson());return {};}
    const auto computed=phase(key+"-compute","compute-only",prepared.value("operationId").toString());
    if(!computed.value("toolOk").toBool()){error=QString::fromUtf8(QJsonDocument(computed).toJson());return {};}
    return computed.value("operationId").toString();
  }
};
}
