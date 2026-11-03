// 层：测试壳
#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QLineEdit>
#include <QComboBox>
#include <QThreadPool>
#include <QElapsedTimer>
#include <qgsgui.h>
#include <qgseditorwidgetregistry.h>
#include <qgsapplication.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsrasterdataprovider.h>
#include <qgsvectorlayer.h>
#include <qgsvectordataprovider.h>
#include "catalog/datacatalog.h"
#include "metadata/paleoprojectstore.h"
#include "metadata/layermanifest.h"
#include "services/seismichorizoncluster.h"
#include "services/seismicunitinput.h"
#include "services/paleotaskservice.h"
#include "services/stratigraphicsubintervals.h"
#include "qgis/qgisprojectservice.h"
#include "qgis/qgislayerservice.h"
#include "qgis/qgisprocessingservice.h"
#include "qgis/mappingartifactwriter.h"
#include "qgis/seismicwellcalibration.h"
#include "services/subintervalfactors.h"
#include "workflow/workflows.h"
#include "workflow/mappingworkbench.h"
#include "workflow/derivedassets.h"
#include "workflow/analysisresultsync.h"
#include "workflow/mockseismicinputs.h"
#include "ui/pages/mappingworkbenchpage.h"
#include "domain/faciescatalog.h"
#include "io/areamodel.h"
#include "seismichorizonfixture.h"
#include <paleo_data/service.h>
#include <cmath>
using namespace paleo::strat;

struct MockUnitFixture {
  QTemporaryDir temp;QString project,source,error;std::string detail;
  paleo::data::DataService ds;paleo::data::StratigraphicScheme scheme;
  DataCatalog catalog;QgisProjectService qgis;PaleoProjectStore store;
  LayerManifest manifest{temp.filePath("layers.sqlite")};QgisLayerService layers{&qgis,&manifest};
  QgisProcessingService processing{&store};ConstraintWorkflow constraints{&processing,&layers};
  PaleoTaskService tasks;MappingWorkbench work{&layers,&processing,&qgis,&constraints};
  QVariantList schema{FaciesCatalog::defaults()[0],FaciesCatalog::defaults()[1]};
  bool init(){
    project=temp.filePath("project");source=temp.filePath("volume.sgy");
    if(!ds.createProject(paleo::area_model::directory(project).toStdString(),"mock",detail)||
        !SeismicHorizonFixture::writeFile(source,SeismicHorizonFixture::buildVolume()))return false;
    paleo::data::ImportOptions options;options.copySources=false;paleo::data::ImportReport report;
    if(!ds.importFile(source.toStdString(),options,report,detail))return false;
    const auto picks=temp.filePath("picks.txt");if(!SeismicHorizonFixture::writeFile(picks,"synthetic"))return false;
    paleo::data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";
    for(int i=0;i<2;++i){paleo::data::WellDataset well;well.wellInfo.wellName="W"+std::to_string(i);well.wellInfo.wellheadX=i*10;well.wellInfo.wellheadY=40;parsed.wells.push_back(well);}
    for(int edge=0;edge<2;++edge){paleo::data::SeismicHorizon surface;surface.horizonName=edge?"T72":"T70";
      surface.timeUnit="ms";surface.timeType="TWT";
      for(int il=100;il<104;++il)for(int xl=2000;xl<2004;++xl)
        surface.points.push_back({il,xl,double((xl-2000)*10),double((il-100)*20),xl==2003||(il==101&&xl==2001)?NAN:80.+edge*80});
      parsed.horizons.push_back(surface);
    }
    long long id=0;if(!ds.importPreparedFile(picks.toStdString(),picks.toStdString(),picks.toStdString(),
      QCryptographicHash::hash("synthetic",QCryptographicHash::Sha256).toHex().toStdString(),parsed,id,detail))return false;
    auto draft=draftScheme("mock","test",{"T70","T72"},{{17,{{"T70",1000.},{"T72",1080.}}}});
    std::vector<paleo::data::DataEntry> surfaces;if(!ds.listData("horizons",0,surfaces,detail))return false;
    for(auto &boundary:draft.boundaries)for(const auto &entry:surfaces)if(entry.name==boundary.name)boundary.seismicHorizonResourceId=entry.id;
    std::vector<paleo::data::FrameworkIssue> issues;if(!ds.commitStratigraphicScheme(draft,{},scheme,issues,detail))return false;
    if(!catalog.open(project,&error)||!qgis.createProject(temp.filePath("project.qgz"))||!manifest.open())return false;
    store.setProjectPaths(qgis.projectPath(),temp.filePath("project.gpkg"),temp.filePath("project.sqlite"));
    constraints.setStore(&store);constraints.setCatalog(&catalog,project);processing.setProject(qgis.project());
    work.setTaskService(&tasks);work.bindCatalog(&catalog,project);work.setAnalysisUnit(unit());
    std::vector<paleo::data::WellSummary> wells;if(!ds.listWells(wells,detail))return false;
    for(const auto &well:wells){CatalogEntity entity;entity.id=QString::fromStdString(well.name).mid(1);entity.name=QString::fromStdString(well.name);
      entity.entityType="well";entity.extra={{"areaModelWellId",well.id}};if(!catalog.addEntity(entity,&error))return false;}
    CatalogAsset asset;asset.id="volume";asset.type="seismic";asset.displayName="volume";
    CatalogVersion version;version.id="volume-v1";version.assetId=asset.id;version.path=source;version.sourceUri=source;
    version.managed=false;version.stage="RAW";version.sha256=DataCatalog::sha256FileHex(source);
    return catalog.addAsset(asset,&error)&&catalog.addVersion(version,&error)&&work.saveFacies(analysisLayerKey(unit()),schema,&error);
  }
  UnitSelection unit(int index=1)const{return makeSelection(scheme,selectorOptions(scheme).first().intervalId,index);}
  QString saveWells(UnitSelection scope,const QString &name="calibration",const QString &label={},bool legacyLocal=false){
    DerivedAssetRegistrar registrar(&catalog,project);const auto stage=registrar.stage("wells_prediction",name,legacyLocal?"wells.geojson":"wells.gpkg",&error);
    QgsVectorLayer points("Point?crs=EPSG:32650&field=id:string&field=facies_code:integer","wells","memory");
    QgsFeatureList features;
    for(int i=0;i<2;++i){QgsFeature feature(points.fields());feature.setAttribute("id",QString::number(i));
      feature.setAttribute("facies_code",i+1);feature.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(i*10,40)));features<<feature;}
    if(!stage.isValid()||!points.dataProvider()->addFeatures(features))return {};
    if(legacyLocal){QVariantList rows;for(int i=0;i<2;++i)rows<<QVariantMap{{"id",QString::number(i)},{"x",i*10.},{"y",40.},{"facies_code",i+1}};
      if(!MappingArtifactWriter::points(stage.absolutePath,rows,&error))return {};
    }else if(!MappingArtifactWriter::vectorSnapshot(&points,stage.absolutePath,&error))return {};
    const auto h=label.isEmpty()?analysisLayerKey(scope):label;
    if(!registrar.commit(stage,{},"fixture",{{"kind","wells_prediction"},{"horizon",h},{"facies",schema},
      {"unitSelection",toMap(scope)},{"mapping_product",true}},&error))return {};
    return stage.versionId;
  }
};
class MockSeismicUnitTest:public QObject {
  Q_OBJECT
private slots:
  void nativeWindowHasSelectedCrsFullGeometryAndMissingMask(){
    MockUnitFixture f;QVERIFY2(f.init(),qPrintable(f.error+QString::fromStdString(f.detail)));
    const auto grid=clusterSeismicStratigraphicWindow(f.project,f.unit(),f.source,{2,7});
    QVERIFY2(grid.error.isEmpty(),qPrintable(grid.error));QVERIFY(QgsCoordinateReferenceSystem::fromWkt(grid.crsWkt)==QgsCoordinateReferenceSystem("EPSG:32650"));
    QCOMPARE(grid.validCells,11);QCOMPARE(grid.cells.size(),16);QCOMPARE(fromMap(grid.parameters.value("unitSelection").toMap()),f.unit());
    QCOMPARE(grid.parameters.value("resampled_samples_per_trace").toInt(),16);
    QCOMPARE(grid.parameters.value("window_type").toString(),QString("stratigraphic_unit"));
    QVERIFY(std::abs(grid.extent.right()-30.-grid.extent.width()/8)<1e-8);
    for(int row=0;row<4;++row)QCOMPARE(grid.cells[row*4+3],-9999); // Missing outer column must not shrink the grid.
    const auto again=clusterSeismicStratigraphicWindow(f.project,f.unit(),f.source,{2,7});QCOMPARE(again.cells,grid.cells);
    const auto last=clusterSeismicStratigraphicWindow(f.project,f.unit(4),f.source,{2,7});QVERIFY2(last.error.isEmpty(),qPrintable(last.error));QVERIFY(last.cells!=grid.cells);
    const auto other=f.temp.filePath("other.sgy");auto bytes=SeismicHorizonFixture::buildVolume();bytes[3600+240]=char(0x41);
    QVERIFY(SeismicHorizonFixture::writeFile(other,bytes));paleo::data::ImportOptions options;options.copySources=false;paleo::data::ImportReport report;
    QVERIFY(f.ds.importFile(other.toStdString(),options,report,f.detail));
    const auto selected=clusterSeismicStratigraphicWindow(f.project,f.unit(),other,{2,7});QVERIFY2(selected.error.isEmpty(),qPrintable(selected.error));
    QCOMPARE(selected.parameters.value("seismic_source").toString(),other);QVERIFY(selected.parameters.value("seismic_volume_id")!=grid.parameters.value("seismic_volume_id"));
    QVERIFY(!clusterSeismicStratigraphicWindow(f.project,f.unit(),{},{2,7}).error.isEmpty());
    const auto stopped=clusterSeismicStratigraphicWindow(f.project,f.unit(),f.source,{2,7},{},[]{return true;});QVERIFY(!stopped.error.isEmpty());QVERIFY(stopped.cells.isEmpty());
  }
  void calibrationRejectsSameLabelFromDifferentSubinterval(){
    MockUnitFixture f;QVERIFY(f.init());const auto key=analysisLayerKey(f.unit());
    QVERIFY(!f.saveWells(f.unit(2),"wrong-unit",key).isEmpty());
    QVERIFY(mockSeismicCalibrationSources(&f.catalog,f.project,key,f.unit()).isEmpty());
    const auto saved=f.saveWells(f.unit());QVERIFY(!saved.isEmpty());
    const auto sources=mockSeismicCalibrationSources(&f.catalog,f.project,key,f.unit());QCOMPARE(sources.size(),1);QCOMPARE(sources[0].toMap().value("version_id").toString(),saved);
  }
  void predictionPublishesNativeRasterPolygonAndDataServiceBinding(){
    MockUnitFixture f;QVERIFY(f.init());const auto saved=f.saveWells(f.unit(),"calibration",{},true);QVERIFY2(!saved.isEmpty(),qPrintable(f.error));
    const auto key=analysisLayerKey(f.unit());QSignalSpy failures(&f.work,&MappingWorkbench::errorOccurred);
    QSignalSpy polygons(&f.work,&MappingWorkbench::polygonizeFinished);
    MappingWorkbenchPage page("predict",&f.work);page.setHorizon(key);auto *kind=page.findChild<QComboBox *>("predictionKind");QVERIFY(kind);kind->setCurrentIndex(kind->findData("seismic_mock"));
    QVERIFY(page.findChild<QLineEdit *>("workbenchHorizonFile")->parentWidget()->isHidden());
    QVERIFY2(f.work.predict(key,"seismic_mock",{"volume"},&f.error,"D61-does-not-exist.dat"),qPrintable(f.error));
    QTRY_VERIFY_WITH_TIMEOUT(!f.work.busy(),20000);QTRY_VERIFY_WITH_TIMEOUT(!f.work.polygonizeBusy(),20000);
    QTRY_COMPARE_WITH_TIMEOUT(polygons.size(),1,5000);QVERIFY2(polygons[0][2].toString().isEmpty(),qPrintable(polygons[0][2].toString()));
    QVERIFY2(failures.isEmpty(),failures.isEmpty()?"":qPrintable(failures[0][0].toString()));
    QString rasterId,polygonId;for(const auto &entry:f.work.products(key)){const auto row=entry.toMap();
      if(row.value("kind")=="seismic_prediction")rasterId=row.value("id").toString();
      if(row.value("kind")=="facies_polygons")polygonId=row.value("id").toString();}
    QVERIFY(!rasterId.isEmpty());QVERIFY(!polygonId.isEmpty());const auto version=f.work.versionForLayer(rasterId);
    QVERIFY(version.extra.value("mock").toBool());QCOMPARE(fromMap(version.extra.value("unitSelection").toMap()),f.unit());
    QCOMPARE(version.extra.value("window_type").toString(),QString("stratigraphic_unit"));QVERIFY(!version.extra.contains("window_half_ms"));QVERIFY(version.parentVersionIds.contains(saved));
    QVERIFY(version.extra.value("wells").toList().first().toMap().value("coordinate_source").toString().startsWith("dataservice_verified_legacy:"));
    auto *raster=qobject_cast<QgsRasterLayer *>(f.layers.instantiate(rasterId));QVERIFY(raster&&raster->isValid());QCOMPARE(raster->crs().authid(),QString("EPSG:32650"));
    bool ok=false;const auto code=raster->dataProvider()->sample(QgsPointXY(0,40),1,&ok);QVERIFY(ok);QVERIFY(code==1||code==2);
    QCOMPARE(syncAnalysisResults(&f.catalog,f.project),QString());std::vector<paleo::data::SubIntervalResultBinding> bindings;
    QVERIFY(f.ds.listSubIntervalResults(f.scheme.id,{},bindings,f.detail));bool found=false;
    for(const auto &binding:bindings)if(binding.assetRef==version.id.toStdString()){found=true;QCOMPARE(binding.kind,std::string("seismic_facies"));QCOMPARE(binding.status,std::string("current"));}
    QVERIFY(found);
    auto changed=f.scheme;changed.divisions.front().fractions={.2,.5,.75};changed.divisions.front().mode="interpreted";
    paleo::data::StratigraphicScheme next;std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY(f.ds.commitStratigraphicScheme(changed,f.scheme.revision.value,next,issues,f.detail));
    QCOMPARE(syncAnalysisResults(&f.catalog,f.project),QString());bindings.clear();
    QVERIFY(f.ds.listSubIntervalResults(f.scheme.id,{},bindings,f.detail));
    for(const auto &binding:bindings)if(binding.assetRef==version.id.toStdString())QCOMPARE(binding.status,std::string("stale"));
  }
  void legacyWellCrsRequiresNativeIdentityAndExactPosition(){
    MockUnitFixture f;QVERIFY(f.init());QVERIFY(!f.saveWells(f.unit(),"legacy",{},true).isEmpty());
    auto sources=mockSeismicCalibrationSources(&f.catalog,f.project,analysisLayerKey(f.unit()),f.unit());QCOMPARE(sources.size(),1);
    const auto file=sources[0].toMap().value("path").toString(),sha=DataCatalog::sha256FileHex(file);
    QStringList versions;QString error;const auto crs=QgsCoordinateReferenceSystem("EPSG:32650").toWkt();
    const auto wells=readSeismicCalibrationWells(sources,f.schema,crs,QgsCoordinateTransformContext{},&versions,&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(wells.size(),2);QCOMPARE(wells[0].x,0.);QCOMPARE(wells[0].y,40.);
    auto source=sources[0].toMap(),positions=source.value("native_positions").toMap(),point=positions.value("0").toMap();
    point.insert("x",1.);positions.insert("0",point);source.insert("native_positions",positions);sources[0]=source;
    QVERIFY(readSeismicCalibrationWells(sources,f.schema,crs,QgsCoordinateTransformContext{},&versions,&error).isEmpty());QVERIFY(!error.isEmpty());
    QCOMPARE(DataCatalog::sha256FileHex(file),sha);
    const auto output=f.temp.filePath("explicit.geojson");
    QVERIFY(MappingArtifactWriter::points(output,{QVariantMap{{"id","W"},{"x",303512.5},{"y",2361325.3}}},crs,&error));
    QgsVectorLayer layer(output,"wells","ogr");QVERIFY(layer.isValid());QCOMPARE(layer.crs().authid(),QString("EPSG:32650"));
    QVERIFY(!MappingArtifactWriter::points(f.temp.filePath("invalid.geojson"),{},"invalid-crs",&error));
  }
  void cancelledPredictionDoesNotPublish(){
    MockUnitFixture f;QVERIFY(f.init());QVERIFY(!f.saveWells(f.unit()).isEmpty());const auto key=analysisLayerKey(f.unit());
    QVERIFY(f.work.predict(key,"seismic_mock",{"volume"},&f.error));f.work.cancelPrediction();QVERIFY(f.tasks.workerPool()->waitForDone(10000));QTest::qWait(50);
    QVERIFY(!f.work.busy());for(const auto &entry:f.work.products(key))QVERIFY(entry.toMap().value("kind")!="seismic_prediction");
  }
  void realCurrentVolumeAndSavedWellCalibration(){
    const auto project=qEnvironmentVariable("PALEO_MOCK_REAL_PROJECT"),source=qEnvironmentVariable("PALEO_MOCK_REAL_SOURCE");
    if(project.isEmpty()||source.isEmpty())QSKIP("Optional user-supplied project and external seismic volume");
    QString error;paleo::data::StratigraphicScheme scheme;QElapsedTimer timer;timer.start();
    QVERIFY2(loadProjectSchemeFromDir(project,{},{},scheme,nullptr,&error),qPrintable(error));
    const auto wanted=qEnvironmentVariable("PALEO_MOCK_REAL_UNIT");UnitSelection unit;
    for(const auto &option:selectorOptions(scheme)){
      const auto candidate=makeSelection(scheme,option.intervalId,1);
      if(wanted.isEmpty()||analysisLayerKey(candidate)==wanted){unit=candidate;break;}
    }
    QVERIFY(unit.isValid());DataCatalog catalog;catalog.setLockedReadOnly(true);QVERIFY2(catalog.open(project,&error),qPrintable(error));
    const auto sources=mockSeismicCalibrationSources(&catalog,project,analysisLayerKey(unit),unit);QVERIFY(!sources.isEmpty());
    const auto schema=sources.first().toMap().value("facies").toList();QVector<int> codes;for(int i=0;i<schema.size();++i)codes<<i+1;
    auto grid=clusterSeismicStratigraphicWindow(project,unit,source,codes,{},[&]{return timer.elapsed()>45000;});
    QVERIFY2(grid.error.isEmpty(),qPrintable(grid.error));QVERIFY(grid.validCells>0);const auto clusteredMs=timer.elapsed();
    QStringList versions;const auto wells=readSeismicCalibrationWells(sources,schema,grid.crsWkt,QgsCoordinateTransformContext{},&versions,&error);
    QVERIFY2(error.isEmpty(),qPrintable(error));QVERIFY(!wells.isEmpty());grid=labelSeismicClusters(std::move(grid),wells,42);
    QVERIFY2(grid.error.isEmpty(),qPrintable(grid.error));QTemporaryDir output;const auto file=output.filePath("mock.tif");
    QVERIFY2(MappingArtifactWriter::raster(file,grid.cells,grid.columns,grid.rows,grid.extent,grid.crsWkt,&error),qPrintable(error));
    QgsRasterLayer raster(file,"mock");QVERIFY(raster.isValid());QVERIFY(raster.crs().isValid());
    qInfo()<<"real Mock"<<analysisLayerKey(unit)<<"cluster_ms"<<clusteredMs<<"total_ms"<<timer.elapsed()<<"valid_cells"<<grid.validCells
      <<"wells"<<wells.size()<<"classes"<<grid.clusterMapping.size()<<"crs"<<raster.crs().authid();
  }
};
int main(int argc,char **argv){
  QTemporaryDir profile;QgsApplication app(argc,argv,true,profile.path());
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH"),true);QgsApplication::init(profile.path());
  QString error;if(!QgsApplication::createDatabase(&error))return 1;app.initQgis();QgsCoordinateReferenceSystem::invalidateCache(true);
  QgsGui::editorWidgetRegistry()->initEditors();MockSeismicUnitTest test;const int result=QTest::qExec(&test,argc,argv);QgsApplication::exitQgis();return result;
}
#include "tst_mockseismicunit.moc"
