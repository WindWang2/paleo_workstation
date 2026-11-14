// 层：功能
#include "tileoperation.h"
#include "ai/onnxpredictionservice.h"
#include "ai/tileinference.h"
#include "algorithms/projectgeotiff.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <gdal.h>
namespace paleo {
namespace {
bool writeProduct(const QString &path,const FrozenTilePlan &plan,GDALDataType type,double nodata,
                  const void *values,const QJsonObject &provenance,QString &error) {
  if(QFileInfo::exists(path)) { error="private_output_already_exists";return false; }
  const auto &grid=plan.raster; auto ds=ProjectGeoTiff::create(path,grid.columns,grid.rows,type);
  if(!ds) {error="tile_output_create_failed";return false;}
  auto transform=grid.transform;
  bool ok=GDALSetGeoTransform(ds,transform.data())==CE_None &&
      GDALSetProjection(ds,grid.projection.toUtf8().constData())==CE_None;
  const auto band=GDALGetRasterBand(ds,1);GDALSetRasterNoDataValue(band,nodata);
  const auto json=QJsonDocument(provenance).toJson(QJsonDocument::Compact);
  GDALSetMetadataItem(ds,"PALEO_PROVENANCE",json.constData(),nullptr);
  ok=ok && GDALRasterIO(band,GF_Write,0,0,grid.columns,grid.rows,const_cast<void *>(values),
                        grid.columns,grid.rows,type,0,0)==CE_None;
  ok=GDALClose(ds)==CE_None && ok;
  if(!ok)error="tile_output_write_failed";
  return ok;
}
}
AlgorithmRunResult computeTilePlan(PaleoOnnxService *onnx,const FrozenTilePlan &plan,const QString &privateDir,
                                   QJsonObject &summary,const std::function<bool()> &cancelled) {
  AlgorithmRunResult result;summary={};
  const auto fail=[&](const QString &code) { result.ok=false;result.error=code;result.artifacts.clear();return result; };
#if !PALEO_HAVE_ORT
  Q_UNUSED(onnx);Q_UNUSED(plan);Q_UNUSED(privateDir);Q_UNUSED(cancelled);return fail("ort_unavailable");
#else
  if(!onnx)return fail("ort_unavailable");
  FrozenTilePlan checked;QString error;
  if(!FrozenTilePlan::fromJson(plan.toJson(),checked,&error))return fail(error);
  if(!QFileInfo(privateDir).isAbsolute() || !QDir().mkpath(privateDir))return fail("private_directory_unavailable");
  FrozenRasterReader reader(plan.raster);
  if(!reader.open(cancelled,&error))return fail(error);
  TileInferenceRequest req;req.model=plan.model;req.expectedModelSha256=plan.modelSha256;
  req.expectedInputSignature=plan.inputSignature;req.expectedOutputSemantics=plan.outputSemantics;req.expectedClassCount=plan.classCount;
  req.expectedFeatureContractVersion=plan.featureContractVersion;req.expectedFeatures=plan.modelFeatures;
  req.requireProbabilityContract=true;
  req.gridRows=plan.raster.rows;req.gridCols=plan.raster.columns;
  req.tileRows=plan.parameters.value("TILE_ROWS").toInt();req.tileCols=plan.parameters.value("TILE_COLUMNS").toInt();
  req.halo=plan.parameters.value("HALO").toInt();
  req.fetch=[&](int r,int c,int rows,int cols,QVector<float> &out,QString &why) {return reader.read(r,c,rows,cols,out,cancelled,&why);};
  TileInferenceResult inferred;
  if(!runTileInference(onnx,req,&inferred,{},cancelled,&error))return fail(error);
  if(!reader.validate(cancelled,&error))return fail(error);
  const auto &grid=inferred.grid;const auto count=grid.argmax.size();
  QVector<quint8> masked(count,255);QVector<float> confidence(count,-9999);
  for(qsizetype i=0;i<count;++i) if(grid.argmax[i]!=255) {
    confidence[i]=grid.confidence[i];
    if(confidence[i]>=plan.parameters.value("LOW_CONFIDENCE").toDouble())masked[i]=grid.argmax[i];
  }
  QJsonObject provenance{{"sourceVersionId",plan.raster.versionId},{"sourceSha256",plan.raster.sha256},
    {"sourceBundleSha256",plan.raster.bundleSha256()},
    {"modelSha256",inferred.modelSha256},{"featureSchemaHash",plan.featureSchemaHash},
    {"parameters",QJsonObject::fromVariantMap(plan.parameters)}};
  struct Product {QString port;GDALDataType type;double nodata;const void *values;};
  const QVector<Product> products{{"OUTPUT",GDT_Byte,255,grid.argmax.constData()},
    {"MASKED",GDT_Byte,255,masked.constData()},{"CONFIDENCE",GDT_Float32,-9999,confidence.constData()}};
  for(const auto &product:products) {
    if(cancelled && cancelled())return fail("cancelled");
    const auto path=QDir(privateDir).absoluteFilePath(product.port+".tif");
    if(!writeProduct(path,plan,product.type,product.nodata,product.values,provenance,error))return fail(error);
    AlgorithmOutputArtifact artifact;artifact.portId=product.port;artifact.absolutePath=path;
    artifact.suggestedAssetType="aifacies";artifact.extra=provenance.toVariantMap();
    if(product.port!="OUTPUT")artifact.parentPortIds={"OUTPUT"};
    result.artifacts.append(artifact);
  }
  if(!reader.validate(cancelled,&error))return fail(error);
  summary={{"rows",grid.rows},{"columns",grid.cols},{"classes",grid.classes},{"tilesDone",inferred.tilesDone},
    {"sourceVersionId",plan.raster.versionId},{"sourceBundleSha256",plan.raster.bundleSha256()},
    {"modelSha256",inferred.modelSha256},{"featureSchemaHash",plan.featureSchemaHash}};
  result.ok=true;return result;
#endif
}
}
