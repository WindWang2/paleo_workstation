// 层：测试壳
#include <QtTest>
#include "workflow/tileoperation.h"
#include "ai/onnxpredictionservice.h"
#include "ai/onnxfixture.h"
#include "ai/tileinference.h"
#include "algorithms/projectgeotiff.h"
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <ogr_spatialref.h>
#include <cpl_conv.h>
#include <algorithm>
#include <cmath>

using namespace paleo;
namespace {
bool writeRaster(const QString &path,float offset=0,GDALDataType type=GDT_Float32) {
  GDALAllRegister();auto ds=ProjectGeoTiff::create(path,4,4,type);if(!ds)return false;
  double transform[6]={100,2,0,200,0,-2};GDALSetGeoTransform(ds,transform);
  OGRSpatialReference srs;srs.SetFromUserInput("EPSG:32650");char *wkt=nullptr;srs.exportToWkt(&wkt);
  GDALSetProjection(ds,wkt);CPLFree(wkt);float values[16];
  GDALSetRasterUnitType(GDALGetRasterBand(ds,1),"dimensionless");
  GDALSetMetadataItem(GDALGetRasterBand(ds,1),"PALEO_FEATURE_CONTRACT",
    R"({"version":1,"kind":"scalar","unit":"dimensionless","domain":"none","preprocessing":"raw_float32_v1"})",nullptr);
  for(int i=0;i<16;++i)values[i]=float(i-8)+offset;
  const auto ok=GDALRasterIO(GDALGetRasterBand(ds,1),GF_Write,0,0,4,4,values,4,4,GDT_Float32,0,0)==CE_None;
  return GDALClose(ds)==CE_None && ok;
}
bool externalMask(const QString &path) {
  const QByteArray previous=CPLGetConfigOption("GDAL_TIFF_INTERNAL_MASK","");
  CPLSetThreadLocalConfigOption("GDAL_TIFF_INTERNAL_MASK","NO");
  auto ds=GDALOpen(path.toUtf8().constData(),GA_Update);bool ok=ds;
  if(ds) {
    ok=GDALCreateDatasetMaskBand(ds,GMF_PER_DATASET)==CE_None;
    quint8 values[16];std::fill(std::begin(values),std::end(values),255);values[0]=0;
    ok=ok && GDALRasterIO(GDALGetMaskBand(GDALGetRasterBand(ds,1)),GF_Write,0,0,4,4,values,4,4,GDT_Byte,0,0)==CE_None;
    ok=GDALClose(ds)==CE_None && ok;
  }
  CPLSetThreadLocalConfigOption("GDAL_TIFF_INTERNAL_MASK",previous.isEmpty()?nullptr:previous.constData());return ok;
}
FrozenRasterInput source(const QString &path) {
  FrozenRasterInput input;input.path=path;input.versionId="exact-raster-v1";
  input.sha256=PaleoOnnxService::sha256OfFile(path);return input;
}
void bindModel(PaleoOnnxService &onnx,const QString &root,int classes=2,const QString &semantics="logits") {
  onnx.setModelRoot(root);ModelRegistryScan registry;registry.manifestFound=true;
  ModelRegistryEntry entry;entry.name="model";entry.file="model.onnx";
  entry.absolutePath=QDir(root).absoluteFilePath(entry.file);entry.sha256Pinned=PaleoOnnxService::sha256OfFile(entry.absolutePath);
  entry.outputSemantics=semantics;entry.classes=classes;entry.featureContractVersion=1;
  entry.features=QJsonArray{QJsonObject{{"kind","scalar"},{"unit","dimensionless"},{"domain","none"},{"preprocessing","raw_float32_v1"}}};
  registry.entries.append(entry);onnx.setModelRegistry(registry);
}
}
class TestTileOperation:public QObject {
  Q_OBJECT
private slots:
  void prepareRejectsActualOutputChannelsEvenWhenManifestClaimsTwo() {
    QTemporaryDir temp;QVERIFY(writeRaster(temp.filePath("source.tif")));
    QVERIFY(OnnxFixtureWriter::writeSeg(temp.filePath("model.onnx"),QVector<float>(512,1),QVector<float>(512,0)));
    PaleoOnnxService onnx;bindModel(onnx,temp.path(),2);FrozenTilePlan plan;QString error;
    QVERIFY(!prepareTilePlan(&onnx,"model",source(temp.filePath("source.tif")),{},plan,{},&error));
    QCOMPARE(error,QString("tile_model_output_classes_out_of_range"));
  }
  void publicModelFailuresDoNotExposeLocalPaths() {
    QTemporaryDir temp;QVERIFY(writeRaster(temp.filePath("source.tif")));
    QFile broken(temp.filePath("model.onnx"));QVERIFY(broken.open(QIODevice::WriteOnly));QVERIFY(broken.write("invalid")>0);broken.close();
    PaleoOnnxService onnx;bindModel(onnx,temp.path());FrozenTilePlan plan;QString error;
    QVERIFY(!prepareTilePlan(&onnx,"model",source(temp.filePath("source.tif")),{},plan,{},&error));
    QVERIFY(error=="model_invalid" || error=="model_load_failed");QVERIFY(!error.contains(temp.path()));
  }
  void strictProbabilitiesKeepBinaryLabelsAndRejectInvalidVectors() {
    TileClassGrid grid;QString error;
    QVERIFY(probabilityGrid({0.1f,0.9f,0.5f},1,1,3,{},&grid,&error));
    QCOMPARE(grid.argmax,QVector<quint8>({0,1,1}));QCOMPARE(grid.confidence[2],0.0f);
    QVERIFY(!probabilityGrid({-0.1f,1.1f},2,1,1,{},&grid,&error));
    QCOMPARE(error,QString("model_probability_out_of_contract"));
    QVERIFY(!probabilityGrid({0.4f,0.4f},2,1,1,{},&grid,&error));
    QCOMPARE(error,QString("model_probability_sum_invalid"));
    QVERIFY(probabilityGrid({0.25f,0.75f},2,1,1,{},&grid,&error));QCOMPARE(grid.argmax[0],quint8(1));
  }
  void specOwnsDefaultsAndAllThreeRequiredPorts() {
    const auto spec=AlgorithmSpecRegistry::specAtVersion("onnx:tile_classification",2);
    QVERIFY(spec.complete);QCOMPARE(spec.outputs.size(),3);
    for(const auto &port:spec.outputs)QVERIFY(port.required);
    QVERIFY(AlgorithmSpecRegistry::specAtVersion(spec.id,1).id.isEmpty());
    QVariantMap defaults;for(const auto &field:spec.params)defaults.insert(field.key,field.defaultValue);
    QCOMPARE(defaults.value("TILE_ROWS").toInt(),128);QCOMPARE(defaults.value("HALO").toInt(),8);
    QCOMPARE(defaults.value("LOW_CONFIDENCE").toDouble(),0.5);
  }
  void frozenSourceAndModelProduceThreeExactGridFiles() {
    QTemporaryDir temp;QVERIFY(writeRaster(temp.filePath("source.tif")));
    QVERIFY(OnnxFixtureWriter::writeSeg(temp.filePath("model.onnx"),{1,-1},{0,0}));
    PaleoOnnxService onnx;bindModel(onnx,temp.path());FrozenTilePlan plan;QString error;
    QVERIFY2(prepareTilePlan(&onnx,"model",source(temp.filePath("source.tif")),{{"TILE_ROWS",2},{"TILE_COLUMNS",2},{"HALO",0}},plan,{},&error),qPrintable(error));
    QVERIFY(!QFileInfo::exists(temp.filePath("outputs")));QCOMPARE(plan.raster.rows,4);
    FrozenTilePlan restored;QVERIFY(FrozenTilePlan::fromJson(plan.toJson(),restored,&error));
    QJsonObject summary;const auto result=computeTilePlan(&onnx,restored,temp.filePath("outputs"),summary,{});
    QVERIFY2(result.ok,qPrintable(result.error));QCOMPARE(result.artifacts.size(),3);
    QCOMPARE(summary.value("modelSha256").toString(),plan.modelSha256);
    for(const auto &artifact:result.artifacts) {
      auto ds=GDALOpen(artifact.absolutePath.toUtf8().constData(),GA_ReadOnly);QVERIFY(ds);
      QCOMPARE(GDALGetRasterXSize(ds),4);QCOMPARE(GDALGetRasterYSize(ds),4);
      double gt[6];QCOMPARE(GDALGetGeoTransform(ds,gt),CE_None);QCOMPARE(gt[0],100.0);QCOMPARE(gt[5],-2.0);
      QCOMPARE(QString::fromUtf8(GDALGetProjectionRef(ds)),plan.raster.projection);
      const auto prov=QJsonDocument::fromJson(GDALGetMetadataItem(ds,"PALEO_PROVENANCE",nullptr)).object();
      QCOMPARE(prov.value("sourceVersionId").toString(),QString("exact-raster-v1"));
      float values[16];QCOMPARE(GDALRasterIO(GDALGetRasterBand(ds,1),GF_Read,0,0,4,4,values,4,4,GDT_Float32,0,0),CE_None);
      if(artifact.portId=="OUTPUT") {QCOMPARE(values[0],1.0f);QCOMPARE(values[15],0.0f);}
      if(artifact.portId=="MASKED")QCOMPARE(values[8],255.0f);
      if(artifact.portId=="CONFIDENCE")QCOMPARE(values[8],0.0f);
      QVERIFY(!PaleoOnnxService::sha256OfFile(artifact.absolutePath).isEmpty());GDALClose(ds);
    }
  }
  void sourceRevisionChangeInvalidatesTheWholeOperation() {
    QTemporaryDir temp;const auto path=temp.filePath("source.tif");QVERIFY(writeRaster(path));
    QVERIFY(OnnxFixtureWriter::writeSeg(temp.filePath("model.onnx"),{1,-1},{0,0}));
    PaleoOnnxService onnx;bindModel(onnx,temp.path());FrozenTilePlan plan;QString error;
    QVERIFY(prepareTilePlan(&onnx,"model",source(path),{},plan,{},&error));
    QFile::remove(path);QVERIFY(writeRaster(path,100));QJsonObject summary;
    const auto result=computeTilePlan(&onnx,plan,temp.filePath("outputs"),summary,{});
    QVERIFY(!result.ok);QCOMPARE(result.error,QString("raster_source_changed"));QVERIFY(result.artifacts.isEmpty());
    QVERIFY(QDir(temp.filePath("outputs")).entryList(QDir::Files).isEmpty());
  }
  void modelReplacementAfterPrepareAndCancellationCannotCompute() {
    QTemporaryDir temp,replaced;const auto path=temp.filePath("source.tif");QVERIFY(writeRaster(path));
    QVERIFY(OnnxFixtureWriter::writeSeg(temp.filePath("model.onnx"),{1,-1},{0,0}));
    QVERIFY(OnnxFixtureWriter::writeSeg(replaced.filePath("model.onnx"),{2,-2},{0,0}));
    PaleoOnnxService onnx;bindModel(onnx,temp.path());FrozenTilePlan plan;QString error;
    QVERIFY(prepareTilePlan(&onnx,"model",source(path),{},plan,{},&error));bindModel(onnx,replaced.path());
    QJsonObject summary;auto result=computeTilePlan(&onnx,plan,temp.filePath("outputs"),summary,{});
    QVERIFY(!result.ok);QCOMPARE(result.error,QString("model_changed_since_prepare"));
    result=computeTilePlan(&onnx,plan,temp.filePath("cancelled"),summary,[]{return true;});
    QVERIFY(!result.ok);QCOMPARE(result.error,QString("cancelled"));QVERIFY(result.artifacts.isEmpty());
  }
  void unknownGridAndTamperedFrozenPlanAreRejected() {
    QTemporaryDir temp;const auto path=temp.filePath("source.tif");QVERIFY(writeRaster(path));
    FrozenRasterInput frozen;QString error;QVERIFY(freezeRasterInput(path,"v1",PaleoOnnxService::sha256OfFile(path),frozen,{},&error));
    auto json=frozen.toJson();json.insert("rows",100000);QVERIFY(!FrozenRasterInput::fromJson(json,frozen,&error));
    auto ds=GDALOpen(path.toUtf8().constData(),GA_Update);QVERIFY(ds);GDALSetProjection(ds,"");GDALClose(ds);
    QVERIFY(!freezeRasterInput(path,"v1",PaleoOnnxService::sha256OfFile(path),frozen,{},&error));
    QCOMPARE(error,QString("raster_crs_unknown"));
  }
  void declaredModelSemanticsCannotBeGuessedOrReplaced() {
    QTemporaryDir temp;const auto path=temp.filePath("source.tif");QVERIFY(writeRaster(path));
    QVERIFY(OnnxFixtureWriter::writeSeg(temp.filePath("model.onnx"),{1,-1},{0,0}));
    PaleoOnnxService onnx;bindModel(onnx,temp.path(),2,"unknown");FrozenTilePlan plan;QString error;
    QVERIFY(!prepareTilePlan(&onnx,"model",source(path),{},plan,{},&error));QCOMPARE(error,QString("tile_model_semantics_unknown"));
    bindModel(onnx,temp.path(),2,"class_index");
    QVERIFY(!prepareTilePlan(&onnx,"model",source(path),{},plan,{},&error));QCOMPARE(error,QString("tile_confidence_output_unavailable"));
    bindModel(onnx,temp.path());QVERIFY(prepareTilePlan(&onnx,"model",source(path),{},plan,{},&error));
    auto json=plan.toJson();auto parameters=json.value("parameters").toObject();parameters.remove("HALO");json.insert("parameters",parameters);
    FrozenTilePlan restored;QVERIFY(!FrozenTilePlan::fromJson(json,restored,&error));
    bindModel(onnx,temp.path(),2,"probabilities");QJsonObject summary;
    const auto result=computeTilePlan(&onnx,plan,temp.filePath("outputs"),summary,{});
    QVERIFY(!result.ok);QCOMPARE(result.error,QString("model_contract_changed_since_prepare"));
  }
  void featureUnitsAreExplicitAndCaseSensitive() {
    QTemporaryDir temp;const auto path=temp.filePath("source.tif");QVERIFY(writeRaster(path));
    QVERIFY(OnnxFixtureWriter::writeSeg(temp.filePath("model.onnx"),{1,-1},{0,0}));
    PaleoOnnxService onnx;bindModel(onnx,temp.path());FrozenTilePlan plan;QString error;
    auto input=source(path);input.catalogFeatureContract={{"version",1},{"kind","horizon_time"},
      {"unit","ms"},{"domain","TWT"},{"preprocessing","raw_float32_v1"}};
    QVERIFY(!prepareTilePlan(&onnx,"model",input,{},plan,{},&error));QCOMPARE(error,QString("source_feature_contract_mismatch"));
    QJsonObject a,b;auto feature=QJsonObject{{"version",1},{"kind","scalar"},{"unit","mV"},{"domain","none"},{"preprocessing","raw_float32_v1"}};
    QVERIFY(normalizeRasterFeatureContract(feature,a));feature.insert("unit","MV");QVERIFY(normalizeRasterFeatureContract(feature,b));QVERIFY(a!=b);
    feature.insert("scale",0.01);QVERIFY(!normalizeRasterFeatureContract(feature,b));
    feature.remove("scale");feature.insert("unit","unknown");QVERIFY(!normalizeRasterFeatureContract(feature,b));
    feature.insert("unit","nA");QVERIFY(normalizeRasterFeatureContract(feature,b));
    auto ds=GDALOpen(path.toUtf8().constData(),GA_Update);QVERIFY(ds);
    GDALSetMetadataItem(GDALGetRasterBand(ds,1),"PALEO_FEATURE_CONTRACT",nullptr,nullptr);GDALSetRasterUnitType(GDALGetRasterBand(ds,1),"");GDALClose(ds);
    QVERIFY(!prepareTilePlan(&onnx,"model",source(path),{},plan,{},&error));QCOMPARE(error,QString("source_feature_contract_unavailable"));
  }
  void sourceNoDataIsResolvedBeforeFloat32Conversion() {
    QTemporaryDir temp;const auto path=temp.filePath("integer.tif");QVERIFY(writeRaster(path,0,GDT_Int32));
    auto ds=GDALOpen(path.toUtf8().constData(),GA_Update);QVERIFY(ds);auto band=GDALGetRasterBand(ds,1);
    qint32 values[16];std::fill(std::begin(values),std::end(values),-10);values[0]=2147483646;values[1]=2147483647;
    GDALSetRasterNoDataValue(band,2147483647);
    QCOMPARE(GDALRasterIO(band,GF_Write,0,0,4,4,values,4,4,GDT_Int32,0,0),CE_None);GDALClose(ds);
    FrozenRasterInput input;QString error;QVERIFY(freezeRasterInput(path,"v",PaleoOnnxService::sha256OfFile(path),input,{},&error));
    FrozenRasterReader reader(input);QVERIFY(reader.open({},&error));QVector<float> read;
    QVERIFY(reader.read(0,0,4,4,read,{},&error));QVERIFY(std::isfinite(read[0]));QVERIFY(std::isnan(read[1]));QCOMPARE(read[2],-10.0f);
  }
  void externalMasksAreConsumedAndTheirBytesArePinned() {
    QTemporaryDir temp;const auto path=temp.filePath("source.tif");QVERIFY(writeRaster(path));QVERIFY(externalMask(path));
    QVERIFY(QFileInfo::exists(path+".msk"));const auto mainSha=PaleoOnnxService::sha256OfFile(path);
    FrozenRasterInput input;QString error;QVERIFY2(freezeRasterInput(path,"v",mainSha,input,{},&error),qPrintable(error));
    QVERIFY(input.companionSha256.contains("suffix:.msk"));FrozenRasterReader reader(input);QVERIFY(reader.open({},&error));
    QVector<float> values;QVERIFY(reader.read(0,0,4,4,values,{},&error));QVERIFY(std::isnan(values[0]));
    auto mask=GDALOpen((path+".msk").toUtf8().constData(),GA_Update);QVERIFY(mask);quint8 valid=255;
    QCOMPARE(GDALRasterIO(GDALGetRasterBand(mask,1),GF_Write,0,0,1,1,&valid,1,1,GDT_Byte,0,0),CE_None);GDALClose(mask);
    QCOMPARE(PaleoOnnxService::sha256OfFile(path),mainSha);QVERIFY(!reader.validate({},&error));QCOMPARE(error,QString("raster_dependencies_changed"));
  }
  void unsupportedScalarConversionsAreRejectedBeforeSampling() {
    QTemporaryDir temp;QString error;FrozenRasterInput input;
    for(const auto type:{GDT_Int64,GDT_UInt64,GDT_CFloat32}) {
      const auto path=temp.filePath(QString::number(int(type))+".tif");QVERIFY(writeRaster(path,0,type));
      QVERIFY(!freezeRasterInput(path,"v",PaleoOnnxService::sha256OfFile(path),input,{},&error));
      QCOMPARE(error,QString("raster_sample_type_unsupported"));
    }
  }
  void relocatingTheSameBundleDoesNotChangeValueIdentity() {
    QTemporaryDir temp;const auto path=temp.filePath("source.tif");QVERIFY(writeRaster(path));QVERIFY(externalMask(path));
    FrozenRasterInput input;QString error;QVERIFY(freezeRasterInput(path,"v",PaleoOnnxService::sha256OfFile(path),input,{},&error));
    const auto identity=input.bundleSha256();const auto relocated=temp.filePath("renamed.tif");
    QVERIFY(QFile::copy(path,relocated));QVERIFY(QFile::copy(path+".msk",relocated+".msk"));input.path=relocated;
    FrozenRasterReader reader(input);QVERIFY2(reader.open({},&error),qPrintable(error));QCOMPARE(input.bundleSha256(),identity);
  }
};
QTEST_GUILESS_MAIN(TestTileOperation)
#include "tst_tile_operation.moc"
