// 层：测试壳
#include <QtTest>
#include <QElapsedTimer>
#include <cmath>
#include <QTemporaryDir>
#include <QFile>
#include <future>
#include <atomic>
#include "seismichorizonfixture.h"
#include "services/seismiclinecache.h"
#include "services/seismicdataaccess.h"
#include "services/seismicdataprojection.h"
#include "services/seismictaskservice.h"
#include "services/paleotaskservice.h"
#include "services/previewdoc.h"
#include "services/sectiondoc.h"
#include "io/areamodel.h"
#include "io/modelhorizonpreview.h"
#include "io/horizonbinner.h"
#include "io/areamodel_conventions.h"

class TestSeismicReads : public QObject {
  Q_OBJECT
private slots:
  void storedHorizonPreviewWorksWithoutExternalFile() {
    QTemporaryDir temp;const auto project=temp.filePath("project"),path=temp.filePath("surface");
    paleo::data::DataService ds;std::string detail;
    QVERIFY(ds.createProject(paleo::area_model::directory(project).toStdString(),"test",detail));
    QVERIFY(ds.setProjectCrs("EPSG:32650","m",detail));
    QFile raw(path);QVERIFY(raw.open(QIODevice::WriteOnly));raw.write("test");raw.close();
    paleo::data::SeismicHorizon horizon;horizon.horizonName="T70";horizon.timeUnit="s";horizon.timeType="OWT";
    horizon.points={{1,1,300000,2300000,1.2},{2,2,301000,2301000,1.4}};
    paleo::data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";parsed.horizons.push_back(horizon);
    long long sourceId=0;const auto hash=QCryptographicHash::hash("test",QCryptographicHash::Sha256).toHex().toStdString();
    QVERIFY2(ds.importPreparedFile(path.toStdString(),path.toStdString(),path.toStdString(),hash,parsed,sourceId,detail),detail.c_str());
    struct Binding{~Binding(){paleo::area_model::unregisterProject(this);}}binding;
    paleo::area_model::registerProject(project,&binding);QVERIFY(QFile::remove(path));
    BinnedHorizon grid;QString error;
    QCOMPARE(readModelHorizonPreview(path,grid,&error),paleo::area_model::Read::Ready);
    QCOMPARE(grid.coordinateSystem,QString("EPSG:32650"));QVERIFY(grid.rows<=512&&grid.cols<=512);
    QCOMPARE(grid.filledCells,2);QCOMPARE(grid.zMin,2400.);QCOMPARE(grid.zMax,2800.);
    QCOMPARE(grid.z.count(-9999.f),grid.rows*grid.cols-2); // Unobserved cells remain missing.
    QCOMPARE(readModelHorizonPreview(path,grid,&error,[]{return true;}),paleo::area_model::Read::Failed);
  }
  void previewResolutionHasIndependentCacheAndActualSamples() {
    QTemporaryDir temp;const auto path=temp.filePath("native.sgy");
    QVERIFY(SeismicHorizonFixture::writeFile(path,SeismicHorizonFixture::buildVolume()));
    PaleoTaskService tasks;seismic::SeismicTaskService service(&tasks);bool done=false,ok=false;
    std::shared_ptr<const seismic::SgySliceImage> preview,native;
    service.startDataSlice(path,seismic::SgySliceType::Time,25,[&](bool success,auto image,const auto&){ok=success;preview=image;done=true;},1,2);
    QTRY_VERIFY_WITH_TIMEOUT(done,5000);QVERIFY(ok&&preview);QCOMPARE(preview->width,2);QCOMPARE(preview->height,2);
    done=false;QVERIFY(service.startDataSlice(path,seismic::SgySliceType::Time,25,[&](bool success,auto image,const auto&){ok=success;native=image;done=true;}));
    QTRY_VERIFY_WITH_TIMEOUT(done,5000);QVERIFY(ok&&native);QCOMPARE(native->width,4);QCOMPARE(native->height,4);
    QCOMPARE(preview->values[0],native->values[0]);QCOMPARE(preview->values[1],native->values[3]);
    QCOMPARE(preview->values[2],native->values[12]);QCOMPARE(preview->values[3],native->values[15]);
  }
  void registeredPlaneSurvivesUnrelatedPublicationButTracksVolumeConventions() {
    QTemporaryDir temp;const auto project=temp.filePath("project"),path=temp.filePath("native.sgy");
    QVERIFY(SeismicHorizonFixture::writeFile(path,SeismicHorizonFixture::buildVolume()));
    paleo::data::DataService ds;std::string detail;
    QVERIFY(ds.createProject(paleo::area_model::directory(project).toStdString(),"test",detail));
    paleo::data::ImportReport report;paleo::data::ImportOptions options;options.copySources=false;
    QVERIFY(ds.importFile(path.toStdString(),options,report,detail));
    struct Binding{~Binding(){paleo::area_model::unregisterProject(this);}}binding;
    paleo::area_model::registerProject(project,&binding);
    PaleoTaskService tasks;seismic::SeismicTaskService service(&tasks);bool done=false,ok=false;
    service.startDataPlane(path,seismic::SgySliceType::Inline,100,[&](bool success,auto image,const auto&,const auto&){ok=success&&image;done=true;});
    QVERIFY(ds.setProjectCrs("EPSG:32650","m",detail)); // DB write while the section task is in flight.
    QTRY_VERIFY_WITH_TIMEOUT(done,10000);QVERIFY(ok);
    std::vector<paleo::data::DataEntry> entries;QVERIFY(ds.listData("volumes",0,entries,detail));
    paleo::data::SeismicVolume volume;QVERIFY(ds.readVolume(entries.front().id,volume,detail));
    done=false;service.startDataSlice(path,seismic::SgySliceType::Inline,100,[&](bool success,auto,const auto&){ok=success;done=true;});
    volume.sampling.timeReference="changed during queued delivery";
    QVERIFY(ds.saveVolume(entries.front().id,volume,detail));
    QTRY_VERIFY_WITH_TIMEOUT(done,10000);QVERIFY(!ok);
  }
  void previewDeliversAmplitudeBeforeOverlayAndDropsSupersededResults() {
    QTemporaryDir temp;const auto path=temp.filePath("native.sgy");
    QVERIFY(SeismicHorizonFixture::writeFile(path,SeismicHorizonFixture::buildVolume()));
    PaleoTaskService tasks;PreviewDocService service(nullptr);service.setTaskService(&tasks);
    QStringList events;QVector<PreviewDocService::SectionDoc> images,complete;
    connect(&service,&PreviewDocService::seismicSectionImageReady,this,[&](const auto&,const auto& doc){
      events.append("image");images.append(doc);
    });
    connect(&service,&PreviewDocService::seismicSectionReady,this,[&](const auto&,const auto& doc){events.append("overlay");complete.append(doc);});
    service.requestSection("preview",{},path,true,{},true,100);
    QTRY_COMPARE_WITH_TIMEOUT(complete.size(),1,5000);
    QCOMPARE(events,QStringList({"image","overlay"}));QCOMPARE(images.size(),1);
    QCOMPARE(images[0].traces.size(),4);QCOMPARE(images[0].traces[0].samples.size(),50);
    QCOMPARE(complete[0].traces[0].samples,images[0].traces[0].samples);QVERIFY(complete[0].horizonsOnly);
    events.clear();images.clear();complete.clear();
    const auto supersede=connect(&service,&PreviewDocService::seismicSectionImageReady,this,[&](const auto&,const auto& doc){
      if(doc.lineNo==100)service.requestSection("preview",{},path,true,{},true,101);
    });
    service.requestSection("preview",{},path,true,{},true,100);
    QTRY_COMPARE_WITH_TIMEOUT(complete.size(),1,5000);
    QCOMPARE(complete[0].lineNo,101);QVERIFY(complete[0].horizonsOnly);
    QCOMPARE(images.size(),2);QCOMPARE(images[0].lineNo,100);QCOMPARE(images[1].lineNo,101);
    QCOMPARE(events,QStringList({"image","image","overlay"}));disconnect(supersede);
    events.clear();images.clear();complete.clear();
    const auto release=connect(&service,&PreviewDocService::seismicSectionImageReady,this,[&](const auto&,const auto&){service.releaseSection("preview");});
    service.requestSection("preview",{},path,true,{},true,102);
    QTRY_COMPARE_WITH_TIMEOUT(images.size(),1,5000);QTest::qWait(50);
    QVERIFY(complete.isEmpty());disconnect(release);
    events.clear();images.clear();complete.clear();
    const auto cancel=connect(&service,&PreviewDocService::seismicSectionImageReady,this,[&](const auto&,const auto&){tasks.tasks().back()->requestCancel();});
    service.requestSection("preview",{},path,true,{},true,103);
    QTRY_COMPARE_WITH_TIMEOUT(complete.size(),1,5000);
    QVERIFY(complete[0].horizonsOnly);QVERIFY(complete[0].horizons.warnings.contains(QString::fromUtf8("地震层位加载已取消")));
    QCOMPARE(complete[0].traces[0].samples,images[0].traces[0].samples);disconnect(cancel);
  }
  void lineCachePreservesValuesWindowsAndSourceChanges() {
    QTemporaryDir temp;const auto path=temp.filePath("native.sgy");
    QVERIFY(SeismicHorizonFixture::writeFile(path,SeismicHorizonFixture::buildVolume()));
    paleo::clearSeismicLineCache();paleo::SeismicDataAccess source;QString error;std::string detail;
    QVERIFY(source.open(path,&error));paleo::data::SectionReadOptions options;options.fullResolution=true;
    paleo::data::SeismicSectionResult first,second;QVERIFY2(source.line(true,100,options,first,detail),detail.c_str());
    QVERIFY(source.line(true,100,options,second,detail));QVERIFY(first.amplitudes==second.amplitudes);
    QCOMPARE(first.columns,4);QCOMPARE(first.samples,50);QCOMPARE(first.amplitudes[22],3.f);
    QCOMPARE(paleo::seismicLineCacheStats().reads,quint64(1));QCOMPARE(paleo::seismicLineCacheStats().hits,quint64(1));
    options.firstTime=options.lastTime=100;QVERIFY(source.line(true,100,options,second,detail));
    QCOMPARE(second.samples,1);QCOMPARE(second.amplitudes[0],3.f);QCOMPARE(second.amplitudes[1],-30.f);
    QVERIFY(!source.line(true,100,options,second,detail,[]{return true;}));QVERIFY(second.amplitudes.empty());
    QVERIFY(!source.line(true,999,options,second,detail));QVERIFY(second.amplitudes.empty());
    const auto stamp=QFileInfo(path).lastModified();QFile file(path);QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.seek(3600+240+25*4));quint32 bits=0;const float replacement=17.f;std::memcpy(&bits,&replacement,4);
    QByteArray value(4,0);qToBigEndian(bits,value.data());QCOMPARE(file.write(value),qint64(4));file.flush();
    QVERIFY(file.setFileTime(stamp,QFileDevice::FileModificationTime));file.close();
    QVERIFY(source.line(true,100,options,second,detail));QCOMPARE(second.amplitudes[0],replacement);
    QVERIFY(paleo::seismicLineCacheStats().bytes<=128ULL*1024*1024);
  }
  void concurrentReadersCoalesceAndCancelIndependently() {
    QTemporaryDir temp;const auto path=temp.filePath("identity");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("source");file.close();
    paleo::clearSeismicLineCache();std::promise<void> entered,release;auto released=release.get_future().share();
    std::atomic_bool cancel=false;std::atomic_int calls=0;
    const paleo::SeismicLineRead read=[&](auto& result,auto&){++calls;entered.set_value();released.wait();result.columns=result.samples=1;result.amplitudes={7.f};return true;};
    const auto query=[&](bool cancellable){paleo::data::SeismicSectionResult result;std::string error;
      const bool ok=paleo::cachedSeismicLine(path,{},true,1,{},result,error,[&]{return cancellable&&cancel.load();},read);
      return std::pair{ok,result.amplitudes};};
    auto first=std::async(std::launch::async,[&]{return query(false);});entered.get_future().wait();
    auto second=std::async(std::launch::async,[&]{return query(true);});
    // Always release the worker before any assertion can unwind its future.
    QElapsedTimer wait;wait.start();while(paleo::seismicLineCacheStats().waits==0&&wait.elapsed()<2000)QTest::qWait(1);
    cancel=true;const bool cancelled=second.wait_for(std::chrono::seconds(2))==std::future_status::ready;
    release.set_value();const auto a=first.get(),b=second.get();
    QVERIFY(cancelled);QVERIFY(a.first);QCOMPARE(a.second,std::vector<float>{7.f});QVERIFY(!b.first);QVERIFY(b.second.empty());QCOMPARE(calls.load(),1);
    int failures=0;for(int i=0;i<2;++i){paleo::data::SeismicSectionResult result;std::string error;
      QVERIFY(!paleo::cachedSeismicLine(path,"failed",true,1,{},result,error,{},[&](auto&,auto&){++failures;return false;}));}
    QCOMPARE(failures,2);
  }
  void timeSliceImageCacheKeepsAsynchronousDelivery() {
    QTemporaryDir temp;const auto path=temp.filePath("native.sgy");
    QVERIFY(SeismicHorizonFixture::writeFile(path,SeismicHorizonFixture::buildVolume()));
    seismic::SgySliceImage reference;QString error;
    QVERIFY2(paleo::readSeismicDataSlice(path,seismic::SgySliceType::Time,25,reference,error),qPrintable(error));
    QVERIFY(!reference.rgba.empty());
    PaleoTaskService tasks;seismic::SeismicTaskService service(&tasks);bool done=false,ok=false;
    std::shared_ptr<const seismic::SgySliceImage> first,second;std::vector<seismic::SeismicTimeTile> tiles;
    connect(&service,&seismic::SeismicTaskService::timeSliceTileReady,this,[&](const auto& tile){tiles.push_back(tile);});
    auto* task=service.startDataSlice(path,seismic::SgySliceType::Time,25,[&](bool success,auto image,const auto&){ok=success;first=image;done=true;},42);
    QVERIFY(task);QTRY_VERIFY_WITH_TIMEOUT(done,5000);QVERIFY(ok&&first);done=false;
    QTRY_COMPARE_WITH_TIMEOUT(tiles.size(),size_t(1),5000);
    QCOMPARE(tiles[0].datasetPath,path);QCOMPARE(tiles[0].sampleIndex,25);QCOMPARE(tiles[0].generation,quint64(42));
    QCOMPARE(tiles[0].x,0);QCOMPARE(tiles[0].y,0);QCOMPARE(tiles[0].completed,1);QCOMPARE(tiles[0].total,1);
    QVERIFY(tiles[0].image&&tiles[0].image->values==reference.values);
    QVERIFY(first->rgba.empty());QVERIFY(first->values==reference.values);
    QCOMPARE(first->valueMin,reference.valueMin);QCOMPARE(first->valueMax,reference.valueMax);
    task=service.startDataSlice(path,seismic::SgySliceType::Time,25,[&](bool success,auto image,const auto&){ok=success;second=image;done=true;});
    QVERIFY(!task);QVERIFY(!done);QTRY_VERIFY(done);QVERIFY(ok);QCOMPARE(first.get(),second.get());
    QVERIFY(service.dataCache().Hits()>0);
    service.dataCache().Clear();done=false;std::shared_ptr<const seismic::SgySliceImage> plane;
    task=service.startDataPlane(path,seismic::SgySliceType::Time,25,[&](bool success,auto image,const auto& axis,const auto&){ok=success;plane=image;done=true;
      QCOMPARE(axis.inlineCount,reference.height);QCOMPARE(axis.xlineCount,reference.width);});
    QVERIFY(task);QTRY_VERIFY_WITH_TIMEOUT(done,5000);QVERIFY(ok&&plane);
    QVERIFY(plane->rgba.empty());QVERIFY(plane->values==reference.values);
    // A metadata change must reject a queued hit rather than publishing the old image.
    done=false;service.startDataSlice(path,seismic::SgySliceType::Time,25,[&](bool success,auto,const auto&){ok=success;done=true;});
    QFile changed(path);QVERIFY(changed.open(QIODevice::ReadWrite));QVERIFY(changed.setFileTime(QDateTime::currentDateTimeUtc().addSecs(2),QFileDevice::FileModificationTime));changed.close();
    QTRY_VERIFY(done);QVERIFY(!ok);
  }
  void horizonCacheFollowsDataServiceChanges() {
    QTemporaryDir temp;const auto project=temp.filePath("project"),path=temp.filePath("native.sgy");
    QVERIFY(SeismicHorizonFixture::writeFile(path,SeismicHorizonFixture::buildVolume()));
    paleo::data::DataService ds;std::string detail;QVERIFY(ds.createProject(paleo::area_model::directory(project).toStdString(),"test",detail));
    paleo::data::ImportReport report;paleo::data::ImportOptions options;options.copySources=false;
    QVERIFY2(ds.importFile(path.toStdString(),options,report,detail),detail.c_str());
    std::vector<paleo::data::DataEntry> volumes;QVERIFY(ds.listData("volumes",0,volumes,detail));
    paleo::data::SeismicVolume volume;const auto id=volumes.front().id;QVERIFY(ds.readVolume(id,volume,detail));
    volume.sampling.timeUnit="ms";volume.sampling.timeType="TWT";volume.sampling.timeReference="test";QVERIFY(ds.saveVolume(id,volume,detail));
    paleo::data::SeismicHorizon horizon;horizon.horizonName="T70";horizon.parentVolumeId=int(id);horizon.timeUnit="ms";horizon.timeType="TWT";
    for(int il=100;il<=103;++il)for(int xl=2000;xl<=2003;++xl)horizon.points.push_back({il,xl,double((xl-2000)*10),double((il-100)*20),100.});
    const auto surface=temp.filePath("surface.txt");QFile raw(surface);QVERIFY(raw.open(QIODevice::WriteOnly));raw.write("synthetic");raw.close();
    paleo::data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";parsed.horizons.push_back(horizon);long long sourceId=0;
    const auto sha=QCryptographicHash::hash("synthetic",QCryptographicHash::Sha256).toHex().toStdString();
    QVERIFY(ds.importPreparedFile(surface.toStdString(),surface.toStdString(),surface.toStdString(),sha,parsed,sourceId,detail));
    std::vector<paleo::data::DataEntry> surfaces;QVERIFY(ds.listData("horizons",0,surfaces,detail));const auto hid=surfaces.front().id;
    struct Binding {~Binding(){paleo::area_model::unregisterProject(this);}}binding;paleo::area_model::registerProject(project,&binding);
    paleo::SeismicDataAccess source;QString error;QVERIFY(source.open(path,&error));paleo::data::SeismicSectionResult line;
    paleo::data::SectionReadOptions read;read.fullResolution=true;QVERIFY(source.line(true,100,read,line,detail));
    const auto first=source.sectionHorizons(line),second=source.sectionHorizons(line);
    QCOMPARE(first.curves.size(),size_t(1));QCOMPARE(first.curves[0].timesMs,second.curves[0].timesMs);QCOMPARE(first.curves[0].timesMs.front(),100.);
    QTest::qWait(5);horizon.horizonName="T70 revised";QVERIFY2(ds.saveHorizonInfo(hid,horizon,detail),detail.c_str());
    const auto revised=source.sectionHorizons(line);QCOMPARE(revised.curves.size(),size_t(1));QCOMPARE(revised.curves[0].name,QString("T70 revised"));QCOMPARE(revised.curves[0].timesMs.front(),100.);
    QVERIFY(source.sectionHorizons(line,[]{return true;}).curves.empty());
    PaleoTaskService tasks;seismic::SeismicTaskService service(&tasks);bool done=false;
    service.startDataHorizonSurfaces(path,[&](bool ok,const auto &names,const auto &grids,const auto &error){
      done=true;
      QVERIFY2(ok,qPrintable(error));QCOMPARE(names,QStringList{"T70 revised"});QCOMPARE(grids.size(),size_t(1));
      QCOMPARE(grids[0].twtMs.size(),size_t(16));for(double time:grids[0].twtMs)QCOMPARE(time,100.);
    });
    QTRY_VERIFY_WITH_TIMEOUT(done,5000);
    QVERIFY(ds.setProjectCrs("EPSG:32650","m",detail));
    volume.geometry.coordinateSystem="EPSG:32650";volume.geometry.coordinateUnit="m";
    QVERIFY(ds.saveVolume(id,volume,detail));horizon.timeUnit.clear();horizon.timeType.clear();
    QVERIFY(ds.saveHorizonInfo(hid,horizon,detail));
    QVERIFY2(paleo::area_model::inheritSurveyHorizonConventions(ds,&error),qPrintable(error));
    QVERIFY(ds.readHorizon(hid,0,1,horizon,detail));QCOMPARE(horizon.points.front().zTime,100.);
    QCOMPARE(horizon.coordinateSystem,std::string("EPSG:32650"));QCOMPARE(horizon.coordinateUnit,std::string("m"));
    QCOMPARE(horizon.timeUnit,std::string("ms"));QCOMPARE(horizon.timeType,std::string("TWT"));
    horizon.parentVolumeId=0;horizon.timeUnit="s";horizon.timeType="OWT";
    QVERIFY(ds.saveHorizonInfo(hid,horizon,detail));QVERIFY(paleo::area_model::inheritSurveyHorizonConventions(ds,&error));
    QVERIFY(ds.readHorizon(hid,0,1,horizon,detail));QCOMPARE(horizon.timeUnit,std::string("s"));QCOMPARE(horizon.timeType,std::string("OWT"));
  }
  void realFastTimePreview() {
    const auto path=qEnvironmentVariable("PALEO_SEISMIC_READ_BENCH_PATH");
    if(path.isEmpty())QSKIP("Optional user-supplied external SEG-Y path");
    const auto project=qEnvironmentVariable("PALEO_SEISMIC_READ_BENCH_PROJECT");
    struct Binding{~Binding(){paleo::area_model::unregisterProject(this);}}binding;
    if(!project.isEmpty())paleo::area_model::registerProject(project,&binding);
    PaleoTaskService tasks;seismic::SeismicTaskService service(&tasks);bool done=false,ok=false;QString error;
    qint64 firstMs=0;int width=0,height=0;QElapsedTimer clock;clock.start();
    service.startDataSlice(path,seismic::SgySliceType::Time,1500,[&](bool success,auto image,const auto &why){
      ok=success;error=why;if(image){width=image->width;height=image->height;}done=true;
    },1,64);
    QTRY_VERIFY_WITH_TIMEOUT(done,60000);QVERIFY2(ok,qPrintable(error));firstMs=clock.elapsed();
    done=false;clock.restart();service.startDataSlice(path,seismic::SgySliceType::Time,1500,[&](bool success,auto,const auto&){ok=success;done=true;},2,64);
    QTRY_VERIFY(done);QVERIFY(ok);
    qInfo()<<"real time overview"<<width<<"x"<<height<<"first_ms"<<firstMs<<"repeat_ms"<<clock.elapsed();
  }
  void realRepeatedLine() {
    const auto path=qEnvironmentVariable("PALEO_SEISMIC_READ_BENCH_PATH");
    if(path.isEmpty())QSKIP("Optional user-supplied external SEG-Y path");
    const auto project=qEnvironmentVariable("PALEO_SEISMIC_READ_BENCH_PROJECT");
    struct Binding { ~Binding(){paleo::area_model::unregisterProject(this);} } binding;
    if(!project.isEmpty())paleo::area_model::registerProject(project,&binding);
    paleo::SeismicDataAccess source;QString error;std::string detail;
    QVERIFY2(source.open(path,&error),qPrintable(error));
    paleo::data::SeismicFileInfo info;QVERIFY2(source.inspect(info,detail),detail.c_str());
    const int requested=qEnvironmentVariableIntValue("PALEO_SEISMIC_READ_BENCH_LINE");
    const int line=requested?requested:info.inlineMin;
    paleo::data::SectionReadOptions options;options.fullResolution=true;options.maxValues=64000000;
    paleo::data::SeismicSectionResult first,second;
    QElapsedTimer clock;clock.start();QVERIFY2(source.line(true,line,options,first,detail),detail.c_str());
    const auto firstMs=clock.elapsed();clock.restart();
    QVERIFY2(source.line(true,line,options,second,detail),detail.c_str());const auto secondMs=clock.elapsed();
    QCOMPARE(first.columns,second.columns);QCOMPARE(first.samples,second.samples);
    QVERIFY(first.amplitudes.size()==second.amplitudes.size());
    for(size_t i=0;i<first.amplitudes.size();++i)
      QVERIFY(first.amplitudes[i]==second.amplitudes[i]||(std::isnan(first.amplitudes[i])&&std::isnan(second.amplitudes[i])));
    qInfo()<<"real native line"<<line<<first.columns<<"x"<<first.samples<<"first_ms"<<firstMs<<"repeat_ms"<<secondMs;
    clock.restart();const auto horizons=source.sectionHorizons(first,[&]{return clock.elapsed()>15000;});
    qInfo()<<"real horizons_ms"<<clock.elapsed()<<"curves"<<horizons.curves.size()<<"warnings"<<horizons.warnings;
    clock.restart();const auto repeatedHorizons=source.sectionHorizons(first);
    qInfo()<<"real horizons_repeat_ms"<<clock.elapsed();QCOMPARE(horizons.curves.size(),repeatedHorizons.curves.size());
    QVERIFY(horizons.warnings.isEmpty());
  }
  void realTimeSlice() {
    const auto path=qEnvironmentVariable("PALEO_SEISMIC_READ_BENCH_PATH");
    if(path.isEmpty())QSKIP("Optional user-supplied external SEG-Y path");
    const auto project=qEnvironmentVariable("PALEO_SEISMIC_READ_BENCH_PROJECT");
    struct Binding {~Binding(){paleo::area_model::unregisterProject(this);}}binding;
    if(!project.isEmpty())paleo::area_model::registerProject(project,&binding);
    paleo::SeismicDataAccess source;QString error;std::string detail;
    QVERIFY2(source.open(path,&error),qPrintable(error));
    paleo::data::SeismicFileInfo info;QVERIFY2(source.inspect(info,detail),detail.c_str());
    const int requested=qEnvironmentVariableIntValue("PALEO_SEISMIC_READ_BENCH_SAMPLE");
    const int sample=requested?requested:info.sampleCount/2;QVERIFY(sample>=0&&sample<info.sampleCount);
    paleo::data::DataService::clearSeismicSliceCache();seismic::SgySliceImage first,second;
    QElapsedTimer clock;clock.start();
    QVERIFY2(paleo::readSeismicDataSlice(path,seismic::SgySliceType::Time,sample,first,error,{},{},false),qPrintable(error));
    const auto firstMs=clock.elapsed();clock.restart();
    qInfo()<<"real time slice first"<<sample<<first.width<<"x"<<first.height<<"first_ms"<<firstMs;
    QVERIFY2(paleo::readSeismicDataSlice(path,seismic::SgySliceType::Time,sample,second,error,{},{},false),qPrintable(error));
    const auto secondMs=clock.elapsed();
    qInfo()<<"real time slice"<<sample<<first.width<<"x"<<first.height<<"first_ms"<<firstMs<<"repeat_ms"<<secondMs<<"rgba_bytes"<<first.rgba.size();
    qInfo()<<"OS file cache is uncontrolled; first_ms is a process-cache miss, not a cold-disk benchmark";
    QVERIFY(first.rgba.empty()&&second.rgba.empty());QVERIFY(first.values==second.values);
    QCOMPARE(first.valueMin,second.valueMin);QCOMPARE(first.valueMax,second.valueMax);
  }
  void realTimeSliceFirstTile() {
    const auto path=qEnvironmentVariable("PALEO_SEISMIC_READ_BENCH_PATH");
    if(path.isEmpty())QSKIP("Optional user-supplied external SEG-Y path");
    const auto project=qEnvironmentVariable("PALEO_SEISMIC_READ_BENCH_PROJECT");
    struct Binding {~Binding(){paleo::area_model::unregisterProject(this);}}binding;
    if(!project.isEmpty())paleo::area_model::registerProject(project,&binding);
    paleo::SeismicDataAccess source;QString error;std::string detail;
    QVERIFY2(source.open(path,&error),qPrintable(error));
    paleo::data::SeismicFileInfo info;QVERIFY2(source.inspect(info,detail),detail.c_str());
    const int requested=qEnvironmentVariableIntValue("PALEO_SEISMIC_READ_BENCH_SAMPLE");
    const int sample=requested?requested:info.sampleCount/2;QVERIFY(sample>=0&&sample<info.sampleCount);
    paleo::data::DataService::clearSeismicSliceCache();seismic::SgySliceImage image;int tiles=0,width=0,height=0;
    QElapsedTimer clock;clock.start();qint64 firstMs=-1;
    const bool ok=paleo::readSeismicDataSlice(path,seismic::SgySliceType::Time,sample,image,error,[&]{return tiles>0;},{},false,
      [&](std::shared_ptr<const seismic::SgySliceImage> tile,int,int,int,int){if(tiles++==0){firstMs=clock.elapsed();width=tile->width;height=tile->height;}});
    QVERIFY(!ok);QCOMPARE(tiles,1);QVERIFY(firstMs>=0);QVERIFY(image.values.empty());
    qInfo()<<"real time slice first tile"<<sample<<width<<"x"<<height<<"first_tile_ms"<<firstMs<<"partial_final_values"<<image.values.size();
  }
  void realPreviewLine() {
    const auto path=qEnvironmentVariable("PALEO_SEISMIC_READ_BENCH_PATH");
    if(path.isEmpty())QSKIP("Optional user-supplied external SEG-Y path");
    const auto project=qEnvironmentVariable("PALEO_SEISMIC_READ_BENCH_PROJECT");
    struct Binding {~Binding(){paleo::area_model::unregisterProject(this);}}binding;
    if(!project.isEmpty())paleo::area_model::registerProject(project,&binding);
    PaleoTaskService tasks;PreviewDocService service(nullptr);service.setTaskService(&tasks);
    QElapsedTimer clock;clock.start();qint64 imageMs=-1,overlayMs=-1;int columns=0,samples=0,curves=0;QStringList warnings;bool overlayOnly=false;
    connect(&service,&PreviewDocService::seismicSectionImageReady,this,[&](const auto&,const auto& doc){
      imageMs=clock.elapsed();columns=int(doc.traces.size());samples=doc.traces.empty()?0:int(doc.traces[0].samples.size());
    });
    connect(&service,&PreviewDocService::seismicSectionReady,this,[&](const auto&,const auto& doc){
      overlayMs=clock.elapsed();curves=int(doc.horizons.curves.size());warnings=doc.horizons.warnings;overlayOnly=doc.horizonsOnly;
    });
    const int line=qEnvironmentVariableIntValue("PALEO_SEISMIC_READ_BENCH_LINE");
    service.requestSection("real-preview",{},path,true,{},true,line);
    QTRY_VERIFY_WITH_TIMEOUT(overlayMs>=0,30000);QVERIFY(imageMs>=0&&overlayMs>=imageMs);QVERIFY(overlayOnly);QVERIFY(warnings.isEmpty());
    qInfo()<<"real preview line"<<line<<columns<<"x"<<samples<<"amplitude_delivery_ms"<<imageMs<<"overlay_delivery_ms"<<overlayMs<<"curves"<<curves;
  }
};
QTEST_GUILESS_MAIN(TestSeismicReads)
#include "tst_seismic_reads.moc"
