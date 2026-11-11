#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <cmath>
#include <paleo_data/service.h>
#include "io/areamodel.h"
#include "io/welllogread.h"
#include "services/previewdoc.h"
#include "domain/wellcompositemodel.h"
#include "catalog/areawellprojection.h"
#include "catalog/areasourceprojection.h"
#include "catalog/documentsourceprojection.h"
#include "catalog/externalsourceverification.h"
#include "services/cataloghealth.h"
#include "catalog/wellavailability.h"
#include "workflow/wellsectionworkflow.h"
#include "workflow/stratigraphyworkflow.h"
#include "workflow/sectionworkbench.h"
#include <QtEndian>
#include "services/projectdata.h"
#include "services/areawellchart.h"
#include "workflow/xmlpreviewsession.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

class TestAreaModel : public QObject {
  Q_OBJECT
private slots:
  void selectedCurvesUseOnlyTheirDepthWindowAndShareTheBudget() {
    QTemporaryDir temp;const auto project=temp.filePath("project"),path=temp.filePath("fixture");
    QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write("fixture"),7LL);file.close();
    paleo::data::DataService ds;std::string detail;
    QVERIFY2(ds.createProject(paleo::area_model::directory(project).toStdString(),"synthetic",detail),detail.c_str());
    paleo::data::ParsedFile parsed;parsed.kind="test";parsed.status="parsed";
    paleo::data::WellDataset well;well.wellInfo.wellName="WINDOW";
    paleo::data::LogSet logs;logs.logSetName="logs";logs.depthInfo.depthType="MD";logs.depthInfo.depthUnit="m";
    paleo::data::LogCurve gr,dt;gr.curveName="GR";dt.curveName="DT";
    for(int i=0;i<9000;++i) {
      logs.samplingAxis.depthValues.push_back(i);
      gr.sampleValues.push_back(i+1.);
      dt.sampleValues.push_back(i+2.);
    }
    paleo::data::LogSet later;later.logSetName="other acquisition";later.depthInfo=logs.depthInfo;
    later.samplingAxis.depthValues={9000,9001};
    paleo::data::LogCurve other;other.curveName="GR";other.sampleValues={11,12};
    later.logCurves.push_back(std::move(other));
    logs.logCurves={gr,dt};well.wellbore.logSets.push_back(std::move(logs));
    well.wellbore.logSets.push_back(std::move(later));parsed.wells.push_back(std::move(well));
    const auto sha=QCryptographicHash::hash("fixture",QCryptographicHash::Sha256).toHex().toStdString();
    long long source=0;
    QVERIFY2(ds.importPreparedFile(path.toStdString(),path.toStdString(),path.toStdString(),sha,parsed,source,detail),detail.c_str());
    std::vector<long long> wells;QVERIFY(ds.listSourceWells(source,wells,detail));QCOMPARE(wells.size(),size_t(1));
    paleo::data::StratigraphicScheme scheme, first, head;scheme.id="alternate";scheme.name="Alternate";
    for(int i=0;i<2;++i){
      paleo::data::StratigraphicBoundary boundary;boundary.name=i?"Base":"Top";boundary.order=i;
      scheme.boundaries.push_back(boundary);
    }
    paleo::data::framework::deriveUnits(scheme);
    paleo::data::WellIntervalOccurrence occurrence;occurrence.wellId=wells.front();
    occurrence.interval=scheme.intervals.front().id;occurrence.top=8190;occurrence.bottom=8191;
    scheme.wellOccurrences.push_back(occurrence);
    std::vector<paleo::data::FrameworkIssue> issues;
    QVERIFY2(ds.commitStratigraphicScheme(scheme,{},first,issues,detail),detail.c_str());
    scheme.wellOccurrences.front().top=8188;
    QVERIFY2(ds.commitStratigraphicScheme(scheme,first.revision.value,head,issues,detail),detail.c_str());
    ds.closeProject();QVERIFY(QFile::remove(path)); // The payload lives in the model.
    AreaWellCurveWindow window;
    window.topMd=8190;window.bottomMd=8191;window.maxSamples=2;window.maxReadBytes=120000;
    QVector<wellsection::Curve> curves;QString error;
    QVERIFY2(readAreaWellSectionCurves(project,wells.front(),{"GR"},curves,&error,{},window),qPrintable(error));
    QCOMPARE(curves.size(),1);QCOMPARE(curves.front().depths,QVector<float>({8190,8191}));
    QCOMPARE(curves.front().values,QVector<float>({8191,8192}));
    QVERIFY(!readAreaWellSectionCurves(project,wells.front(),{"GR","DT"},curves,&error,{},window));
    QVERIFY(curves.isEmpty());QVERIFY(!error.isEmpty());
    auto outside=window;outside.topMd=9000;outside.bottomMd=9001;
    QVERIFY(!readAreaWellSectionCurves(project,wells.front(),{"GR"},curves,&error,{},outside));
    QVERIFY(curves.isEmpty());QVERIFY(!error.isEmpty());
    QVERIFY(ds.openProjectReadOnly(paleo::area_model::directory(project).toStdString(),detail));
    AreaWellChartQuery query;query.curves=window;query.curveNames=QStringList{"GR"};
    query.context.emplace();query.context->documentId="fixture";query.context->modelStoreId="area-model";
    query.context->schemeId=QString::fromStdString(first.id);
    query.context->schemeRevision=QString::fromStdString(first.revision.value);
    WellComposite::ComprehensiveWellData chart;
    QVERIFY2(readAreaWellChart(ds,wells.front(),chart,&error,{},&query),qPrintable(error));
    QCOMPARE(chart.continuousCurves.size(),1);
    QCOMPARE(chart.formationIntervals.size(),1);QCOMPARE(chart.formationIntervals.front().topDepth,8190.f);
    query.context->schemeRevision+="-unknown";
    QVERIFY(!readAreaWellChart(ds,wells.front(),chart,&error,{},&query));
    QVERIFY(chart.isEmpty());QVERIFY(!error.isEmpty());
  }
  void timeDepthOnlyWellsStayVisibleButDoNotEnterAnalysisChoices() {
    QTemporaryDir temp; const auto project=temp.filePath("project");
    paleo::data::DataService ds;std::string error;
    QVERIFY2(ds.createProject(paleo::area_model::directory(project).toStdString(),"synthetic",error),error.c_str());
    const auto path=temp.filePath("source.txt");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");file.close();
    paleo::data::ParsedFile parsed;parsed.path=path.toStdString();parsed.kind="test";parsed.status="parsed";
    for(int n=0;n<3;++n) {
      paleo::data::WellDataset w;w.wellInfo.wellName=std::string("SYNTH-")+std::to_string(n);
      w.wellInfo.wellheadX=100.*n;w.wellInfo.wellheadY=200.;
      w.defaultDepthInfo.depthType="MD";w.defaultDepthInfo.depthUnit="m";
      if(n==0) {paleo::data::TimeDepthTable td;td.relationName="checkshot";td.depthTimePairs={{100,200},{200,300}};w.timeDepthRelations.push_back(td);}
      if(n==1) {paleo::data::LogSet logs;logs.logSetName="logs";logs.depthInfo=w.defaultDepthInfo;logs.samplingAxis.depthValues={100,101};
        auto missing=logs;missing.logSetName="empty acquisition";paleo::data::LogCurve empty;empty.curveName="GR";empty.curveUnit="API";empty.sampleValues={-999.25,-999.25};missing.logCurves.push_back(empty);w.wellbore.logSets.push_back(missing);
        paleo::data::TimeDepthTable td;td.relationName="usable checkshot";td.depthInfo=w.defaultDepthInfo;
        td.timeUnit="ms";td.timeType="TWT";td.timeReference="synthetic datum";td.depthTimePairs={{100,200},{200,300}};w.timeDepthRelations.push_back(td);
        paleo::data::LogCurve curve;curve.curveName="GR";curve.curveUnit="API";curve.sampleValues={10,20};logs.logCurves.push_back(curve);w.wellbore.logSets.push_back(logs);
        paleo::data::IntervalTrack track;track.trackName="组";track.trackType="地层";
        paleo::data::GeologicalInterval interval;interval.intervalName="F1";interval.topDepth=100;interval.bottomDepth=200;track.intervals.push_back(interval);w.wellbore.intervalTracks.push_back(track);}
      if(n==2) {paleo::data::IntervalTrack track;track.trackName="岩性";track.trackType="岩性";track.depthInfo=w.defaultDepthInfo;
        paleo::data::GeologicalInterval row;row.intervalName="砂岩";row.topDepth=100;row.bottomDepth=120;track.intervals.push_back(row);w.wellbore.intervalTracks.push_back(track);}
      parsed.wells.push_back(w);
    }
    const auto sha=QCryptographicHash::hash("fixture",QCryptographicHash::Sha256).toHex().toStdString();
    long long source=0;QVERIFY2(ds.importPreparedFile(path.toStdString(),path.toStdString(),path.toStdString(),sha,parsed,source,error),error.c_str());
    QHash<qlonglong,WellDataAvailability> support;QString detail;QVERIFY(readWellDataAvailability(ds,support,&detail));
    ds.closeProject();QVERIFY(QFile::remove(path));
    DataCatalog catalog;QVERIFY(catalog.open(project,&detail));QVERIFY(registerAreaWells(catalog,&detail));
    QCOMPARE(catalog.entities("well").size(),3);
    const auto td=catalog.entityById(catalog.wellsMatchingName("SYNTH-0").front());
    QVERIFY(!wellSupportsAnalysis(&catalog,td));QVERIFY(td.extra.value("wellDataSummary").toString().contains(QStringLiteral("时深")));
    WellSectionWorkflow sections(&catalog);QCOMPARE(sections.wellChoices().size(),1);
    QCOMPARE(sections.wellChoices().front().name,QString("SYNTH-1"));
    SectionWorkbench seismic(&catalog);QCOMPARE(seismic.wells().size(),1);
    QCOMPARE(seismic.wellSnapWells(true).size(),1);QCOMPARE(seismic.wellSnapWells().size(),3);
    bool ready=false;SectionWorkbench::PreviewData preview;
    seismic.requestPreviewData([&](const auto &value){preview=value;ready=true;},false);
    QTRY_VERIFY(ready);QCOMPARE(preview.wells.size(),1);QVERIFY(preview.sectionWells.empty());
    ProjectDataFacade facade;facade.setCatalog(&catalog,project);
    const auto tops=facade.topsFor(catalog.wellsMatchingName("SYNTH-1").front());
    QVERIFY(std::any_of(tops.begin(),tops.end(),[](const auto &t){return t.horizon=="F1"&&t.md==100.;}));
    QVERIFY(std::any_of(tops.begin(),tops.end(),[](const auto &t){return t.horizon==QStringLiteral("F1底")&&t.md==200.;}));
    QSignalSpy sectionReady(&sections,&WellSectionWorkflow::sectionReady);
    sections.request({catalog.wellsMatchingName("SYNTH-1").front()},{});
    QCOMPARE(sectionReady.size(),1);
    const auto restored=qvariant_cast<QVector<wellsection::Well>>(sectionReady.front().at(1));
    QCOMPARE(restored.size(),1);
    QVector<wellsection::Curve> selected;
    QVERIFY(readAreaWellSectionCurves(project,catalog.entityById(restored.front().id).extra.value("areaModelWellId").toLongLong(),{"GR"},selected,&detail));
    QCOMPARE(selected.size(),1);QCOMPARE(selected.front().values,QVector<float>({10,20}));
    QVERIFY(std::any_of(restored.front().tops.begin(),restored.front().tops.end(),[](const auto &t){return t.name=="F1"&&t.md==100.;}));
    StratigraphyWorkflow stratigraphy(nullptr);stratigraphy.setProject(project,true);
    QCOMPARE(stratigraphy.wells().size(),1);QCOMPARE(stratigraphy.wells().front().name,QString("SYNTH-1"));
  }
  void reimportedSourcesDoNotRestoreInactiveBatchIdentity() {
    QTemporaryDir temp; const auto input=temp.filePath("input"); QVERIFY(QDir().mkpath(input));
    const auto path=input+"/one.las"; QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\n~Curve\nMD.M\nGR.API\n~Ascii\n100 10\n101 20\n"); file.close();
    const auto project=temp.filePath("project"); QString error; paleo::data::ImportReport report;
    QVERIFY2(paleo::area_model::importDirectory(project,input,report,&error),qPrintable(error));
    DataCatalog catalog; QVERIFY(catalog.open(project,&error));
    QVERIFY(registerAreaWells(catalog,&error)); QVERIFY(registerAreaSources(catalog,&error));
    QCOMPARE(catalog.versions().size(),1);
    const auto original=catalog.versions().front();
    QVERIFY(!catalog.linksForAsset(original.assetId).isEmpty());
    paleo::data::DataService ds; std::string detail;
    QVERIFY(ds.openProject(paleo::area_model::directory(project).toStdString(),detail));
    std::vector<paleo::data::SourceEntry> sources; QVERIFY(ds.listSources(sources,detail)); QCOMPARE(sources.size(),size_t(1));
    const auto old=sources.front().id;
    QCOMPARE(original.extra.value("areaModelSourceId").toLongLong(),old);
    QVERIFY2(ds.reimportSource(old,{},report,detail),detail.c_str());
    QVERIFY(ds.listSources(sources,detail)); QCOMPARE(sources.size(),size_t(2));
    qlonglong current=0;
    for(const auto &s:sources) { if(s.id==old) QVERIFY(!s.active); if(s.active) current=s.id; }
    QVERIFY(current>old);
    std::vector<paleo::data::SourceIndexEntry> index; QVERIFY(ds.listSourceIndexEntries(index,detail));
    for(const auto &s:index) QCOMPARE(s.active,s.id==current);
    QString replacementId;
    for(int retry=0;retry<3;++retry) {
      QVERIFY2(registerAreaSources(catalog,&error),qPrintable(error));
      const auto versions=catalog.versions();
      QCOMPARE(versions.size(),2);
      const auto retired=catalog.versionById(original.id);
      QCOMPARE(retired.extra.value("areaModelSourceId").toLongLong(),old);
      const auto observation=retired.extra.value("areaModelObservation").toMap();
      QVERIFY(observation.contains("active"));
      QVERIFY(!observation.value("active").toBool());
      const auto retiredLinks=catalog.linksForAsset(original.assetId);
      QVERIFY(!retiredLinks.isEmpty());
      for(const auto &link:retiredLinks) QVERIFY(link.unresolved && !link.isPrimary);

      const auto &replacement=versions.front().id==original.id ? versions.back() : versions.front();
      QVERIFY(replacement.id!=original.id);
      QCOMPARE(replacement.extra.value("areaModelSourceId").toLongLong(),current);
      QVERIFY(replacement.extra.value("areaModelObservation").toMap().value("active").toBool());
      QVERIFY(!catalog.linksForAsset(replacement.assetId).isEmpty());
      if(retry==0) replacementId=replacement.id;
      QCOMPARE(replacement.id,replacementId);
    }
    const auto projection=paleo::document::projectDocumentSources(catalog,"fixture");
    QVERIFY2(projection.error.isEmpty(),qPrintable(projection.error));
    const auto retired=projection.records.value(projection.versionKeys.value(original.id));
    const auto replacement=projection.records.value(projection.versionKeys.value(replacementId));
    QCOMPARE(retired.ref.sourceId,old);
    QCOMPARE(retired.state.code,QStringLiteral("model_source_inactive"));
    QCOMPARE(replacement.ref.sourceId,current);
    QCOMPARE(replacement.state.model,paleo::document::ModelAvailability::Ready);
    QVERIFY(QFileInfo::exists(path));
  }
  void seismicQuickFingerprintUsesModelVerification() {
    QTemporaryDir temp; const auto input=temp.filePath("input"); QVERIFY(QDir().mkpath(input));
    const auto path=input+"/synthetic.sgy"; QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    QByteArray header(3600,'\0');
    qToBigEndian(quint16(2000),header.data()+3216); qToBigEndian(quint16(8),header.data()+3220);
    qToBigEndian(quint16(5),header.data()+3224); qToBigEndian(quint16(1),header.data()+3502);
    file.write(header);
    for (int i=0;i<4;++i) {
      QByteArray trace(272,'\0'); qToBigEndian(quint16(8),trace.data()+114);
      qToBigEndian(quint16(2000),trace.data()+116);
      qToBigEndian(quint32(100+i/2),trace.data()+188); qToBigEndian(quint32(200+i%2),trace.data()+192);
      file.write(trace);
    }
    file.close();
    const auto project=temp.filePath("project"); QString error; paleo::data::ImportReport report;
    QVERIFY2(paleo::area_model::importDirectory(project,input,report,&error),qPrintable(error));
    QVERIFY(report.hashedBytes<=65536); QCOMPARE(report.copyMs,0LL);
    DataCatalog catalog; QVERIFY(catalog.open(project,&error)); QVERIFY(registerAreaSources(catalog,&error));
    QCOMPARE(catalog.versions().size(),1); auto version=catalog.versions().front();
    QVERIFY(version.sha256.startsWith("quick:"));
    QVERIFY2(catalog.verifyExternalVersionSha(version,&error),qPrintable(error));
    QVERIFY(paleo::health::verifyExternalShas({version},{},{},project).isEmpty());
    bool mismatch=true;
    QVERIFY(verifyExternalSourceFile(project,version,&error,{},&mismatch)); QVERIFY(!mismatch);
    auto invalid=version; invalid.extra.clear();
    QVERIFY(!verifyExternalSourceFile(project,invalid,&error));
    QVERIFY(!verifyExternalSourceFile({},version,&error));
    invalid=version; invalid.sha256="quick:wrong";
    QVERIFY(!verifyExternalSourceFile(project,invalid,&error,{},&mismatch)); QVERIFY(mismatch);
    QVERIFY(!verifyExternalSourceFile(project,version,&error,[]{return true;}));
    QVERIFY(file.open(QIODevice::Append)); file.write("changed"); file.close();
    QVERIFY(!verifyExternalSourceFile(project,version,&error,{},&mismatch)); QVERIFY(mismatch);
    QCOMPARE(paleo::health::verifyExternalShas({version},{},{},project).size(),1);
  }
  void wellChartCombinesStoredSourcesAndCoordinates() {
    QTemporaryDir temp; const QString input=temp.filePath("input");QVERIFY(QDir().mkpath(input));
    const auto write=[&](const QString &name,const QByteArray &bytes){QFile file(input+"/"+name);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();};
    QVERIFY(write("one.las","~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\n~Curve\nMD.M\nGR.API\n~Ascii\n100 10\n101 20\n"));
    QVERIFY(write("two.las","~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\n~Curve\nMD.FT\nRT.OHMM\n~Ascii\n100 30\n101 40\n"));
    const QString project=temp.filePath("project");QString error;paleo::data::ImportReport report;
    QVERIFY2(paleo::area_model::importDirectory(project,input,report,&error),qPrintable(error));
    paleo::data::DataService service;std::string detail;
    QVERIFY(service.openProject(paleo::area_model::directory(project).toStdString(),detail));
    QVERIFY(service.setProjectCrs("EPSG:4326","degree",detail));
    std::vector<paleo::data::WellInfoRecord> wells;QVERIFY(service.listWellInfos(wells,detail));QCOMPARE(wells.size(),size_t(1));
    auto info=wells.front().value;info.wellheadX=0;info.wellheadY=300000;info.coordinateSystem="EPSG:32649";info.coordinateUnit="m";
    QVERIFY(service.saveWellInfo(wells.front().id,info,detail));
    std::vector<paleo::data::DataEntry> curves;QVERIFY(service.listData("curves",wells.front().id,curves,detail));QCOMPARE(curves.size(),size_t(2));
    QVERIFY(service.updateCurveValue(curves.front().id,0,99,detail));
    const auto source=curves.front().sourceId;service.closeProject();
    QVERIFY(QFile::remove(input+"/one.las"));QVERIFY(QFile::remove(input+"/two.las"));
    PreviewDocService::WorkbookPreview table;WellComposite::ComprehensiveWellData chart;bool ready=false;
    QVERIFY(PreviewDocService::areaPreviewSource(project,source,&table,&chart,&ready,&error));
    QVERIFY2(ready,qPrintable(error));QVERIFY(table.ok);QCOMPARE(chart.continuousCurves.size(),2);
    QCOMPARE(chart.continuousCurves.front().values.front(),99.f);QCOMPARE(chart.x,0.0);QCOMPARE(chart.y,300000.0);
    QCOMPARE(chart.coordinateSystem,QString("EPSG:32649"));QCOMPARE(chart.coordinateUnit,QString("m"));
    QCOMPARE(chart.projectCoordinateSystem,QString("EPSG:4326"));QCOMPARE(chart.projectCoordinateUnit,QString("degree"));
    QVERIFY(qAbs(chart.continuousCurves.back().depths.front()-30.48)<1e-4);
    QVERIFY(chart.continuousCurves.front().modelSourceId!=chart.continuousCurves.back().modelSourceId);
    XmlPreviewSession session;QSignalSpy loaded(&session,&XmlPreviewSession::ready);
    session.open(input+"/one.las",{},project,source);
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(),1,10000);
    const auto snapshot=qvariant_cast<XmlPreviewData>(loaded.front().front());
    QVERIFY2(snapshot.chartOk,qPrintable(snapshot.chartError));
    QCOMPARE(snapshot.chart.continuousCurves.size(),2);
    QCOMPARE(snapshot.chart.projectCoordinateSystem,QString("EPSG:4326"));
    // Missing declarations inherit the project convention without changing XY.
    QVERIFY(service.openProject(paleo::area_model::directory(project).toStdString(),detail));
    info.coordinateSystem.clear();info.coordinateUnit.clear();QVERIFY(service.saveWellInfo(wells.front().id,info,detail));service.closeProject();
    QVERIFY(PreviewDocService::areaPreviewSource(project,source,nullptr,&chart,&ready,&error));QVERIFY(ready);
    QCOMPARE(chart.coordinateSystem,QString("EPSG:4326"));QCOMPARE(chart.coordinateUnit,QString("degree"));
    QCOMPARE(chart.projectCoordinateSystem,QString("EPSG:4326"));QCOMPARE(chart.x,0.0);QCOMPARE(chart.y,300000.0);
    QVERIFY(PreviewDocService::areaPreviewSource(project,source,nullptr,&chart,&ready,&error,999999));
    QVERIFY(!ready);QVERIFY(chart.isEmpty());QVERIFY(!error.isEmpty());
    QVERIFY(!PreviewDocService::areaPreviewSource(temp.filePath("missing"),source,nullptr,&chart,&ready,&error));
    QVERIFY(!ready);QVERIFY(chart.isEmpty());
  }
  void coordinateCsvBuildsDomainWells() {
    QTemporaryDir temp; const QString input=temp.filePath("input"); QVERIFY(QDir().mkpath(input));
    QFile f(input + "/coordinates.csv"); QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("WELL,X,Y\nSYNTH-1,100,200\nSYNTH-2,0,500\nTOTAL,,\n"); f.close();
    const QString project=temp.filePath("project"); QString error; paleo::data::ImportReport report;
    QVERIFY2(paleo::area_model::importDirectory(project, input, report, &error), qPrintable(error));
    paleo::data::DataService reader; std::string detail;
    QVERIFY(reader.openProjectReadOnly(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    std::vector<paleo::data::WellInfoRecord> rows; QVERIFY(reader.listWellInfos(rows, detail));
    QCOMPARE(rows.size(), size_t(2)); QCOMPARE(rows[0].value.wellheadX, 100.0); QCOMPARE(rows[0].value.wellheadY, 200.0);
    QCOMPARE(rows[1].value.wellheadX, 0.0); QCOMPARE(rows[0].value.coordinateSystem,std::string("EPSG:32650"));
    QCOMPARE(rows[0].value.coordinateUnit,std::string("m"));
    paleo::data::ProjectCrsInfo crs;QVERIFY(reader.readProjectCrsInfo(crs,detail));QCOMPARE(crs.selectionBasis,std::string("default"));
    DataCatalog catalog; QVERIFY(catalog.open(project, &error)); QVERIFY(registerAreaWells(catalog, &error));
    QCOMPARE(catalog.entities("well").size(), 2); QCOMPARE(catalog.entities("well")[1].surfaceY, 500.0);
  }
  void sourceIndexDoesNotReadOriginalFiles() {
    QTemporaryDir temp; const QString input = temp.filePath("input"); QVERIFY(QDir().mkpath(input));
    const QString path = input + "/well.las"; QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\nNULL. -999.25\n~Curve\nDEPT.M : depth\nGR.API : gamma\n~Ascii\n100 10\n101 20\n"); file.close();
    const QString project = temp.filePath("project"); QString error; paleo::data::ImportReport report;
    QVERIFY2(paleo::area_model::importDirectory(project, input, report, &error), qPrintable(error));
    paleo::data::DataService reader; std::string detail;
    QVERIFY(reader.openProjectReadOnly(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    std::vector<paleo::data::SourceIndexEntry> sources; QVERIFY(reader.listSourceIndexEntries(sources, detail));
    QCOMPARE(sources.size(), size_t(1)); QVERIFY(!sources.front().sha256.empty()); reader.closeProject();

    DataCatalog catalog; QVERIFY(catalog.open(project, &error)); QVERIFY(registerAreaWells(catalog, &error));
    QVERIFY(QFile::remove(path));
    QVERIFY2(registerAreaSources(catalog, &error), qPrintable(error));
    QCOMPARE(catalog.assets().size(), 1); QCOMPARE(catalog.assets().first().type, QString("well_log"));
    QCOMPARE(catalog.versions().size(), 1);
    const auto version = catalog.versions().first();
    QCOMPARE(version.sha256, QString::fromUtf8(sources.front().sha256));
    QCOMPARE(version.extra.value("areaModelSourceId").toLongLong(), sources.front().id);
    QVERIFY(!version.managed);
    const auto links = catalog.linksForAsset(version.assetId);
    QCOMPARE(links.size(), 1); QCOMPARE(links.first().role, QString("well_log"));
    QVERIFY2(registerAreaSources(catalog, &error), qPrintable(error));
    QCOMPARE(catalog.assets().size(), 1); QCOMPARE(catalog.versions().size(), 1);
    QCOMPARE(catalog.linksForAsset(version.assetId).size(), 1);
  }
  void wellCoordinatesComeFromAreaModel() {
    QTemporaryDir temp; const QString project = temp.filePath("project"); QString error;
    DataCatalog catalog; QVERIFY2(catalog.open(project, &error), qPrintable(error));
    CatalogEntity old; old.id="well-existing"; old.entityType="well"; old.name="SYNTH-1";
    old.hasSurface=true; old.surfaceX=11; old.surfaceY=22; old.coordinateStatus="ok";
    QVERIFY(catalog.addEntity(old, &error));
    QVERIFY(paleo::area_model::ensureProject(project, &error));
    paleo::data::DataService model; std::string detail;
    QVERIFY(model.openProject(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    paleo::data::WellInfo info; info.wellName="SYNTH-1"; info.wellheadX=100; info.wellheadY=200;
    info.coordinateSystem="EPSG:32649"; info.coordinateUnit="m"; info.totalDrilledDepth=3000;
    long long id=0; QVERIFY(model.createWell(info, id, detail));
    paleo::data::WellInfo absent; absent.wellName="SYNTH-2"; long long absentId=0;
    QVERIFY(model.createWell(absent, absentId, detail));
    QVERIFY2(registerAreaWells(catalog, &error), qPrintable(error));
    QCOMPARE(catalog.entities("well").size(), 2);
    QCOMPARE(catalog.entityById(old.id).surfaceX, 100.0);
    QCOMPARE(catalog.entityById(old.id).extra.value("wellCoordinatesCrs").toString(), QString("EPSG:32649"));
    ProjectDataFacade facade; facade.setCatalog(&catalog, project);
    info.wellheadX=0; info.wellheadY=250; info.wellName="SYNTH-RENAMED";
    QVERIFY(model.saveWellInfo(id, info, detail));
    QCOMPARE(catalog.entityById(old.id).surfaceX, 0.0); QCOMPARE(catalog.entityById(old.id).surfaceY, 250.0);
    QCOMPARE(catalog.entityById(old.id).name, QString("SYNTH-RENAMED"));
    QCOMPARE(facade.wells().first().surfaceY, 250.0);
    QVERIFY(!catalog.entities("well").last().hasSurface);
    const QString geojson=temp.filePath("wells.geojson"); QVERIFY(catalog.writeWellsGeoJson(geojson, &error));
    QFile f(geojson); QVERIFY(f.open(QIODevice::ReadOnly));
    const auto features=QJsonDocument::fromJson(f.readAll()).object()["features"].toArray();
    QCOMPARE(features.size(), 1); QCOMPARE(features[0].toObject()["geometry"].toObject()["coordinates"].toArray()[1].toDouble(), 250.0);
    // Removal of model coordinates cannot fall back to stale catalog coordinates.
    info.wellheadX=paleo::data::Missing; info.wellheadY=paleo::data::Missing;
    QVERIFY(model.saveWellInfo(id, info, detail)); QVERIFY(!catalog.entityById(old.id).hasSurface);
    QVERIFY(catalog.entityById(old.id).coordinateStatus == "missing");
  }
  void wellRegistrationOnlyPersistsChanges() {
    QTemporaryDir temp; const auto project = temp.filePath("project"); QString error;
    DataCatalog catalog; QVERIFY2(catalog.open(project, &error), qPrintable(error));
    paleo::data::DataService model; std::string detail;
    QVERIFY2(model.createProject(paleo::area_model::directory(project).toStdString(), "registration", detail), detail.c_str());
    paleo::data::WellInfo info; info.wellName="SYNTH-1"; info.wellheadX=100; info.wellheadY=200;
    info.totalDrilledDepth=1000; info.coordinateSystem="EPSG:32650"; info.coordinateUnit="m";
    long long modelId=0; QVERIFY2(model.createWell(info, modelId, detail), detail.c_str());
    paleo::data::WellInfo missing; missing.wellName="SYNTH-MISSING";
    long long missingId=0; QVERIFY2(model.createWell(missing, missingId, detail), detail.c_str());
    QVERIFY2(registerAreaWells(catalog, &error), qPrintable(error));
    const auto ids=catalog.wellsMatchingName("SYNTH-1"); QCOMPARE(ids.size(), 1);
    const auto missingIds=catalog.wellsMatchingName("SYNTH-MISSING"); QCOMPARE(missingIds.size(), 1);
    const auto sequence=catalog.mutationSeq();
    QVERIFY2(registerAreaWells(catalog, &error), qPrintable(error));
    QCOMPARE(catalog.mutationSeq(), sequence);
    const auto absent=catalog.storedEntityById(missingIds.front());
    QVERIFY(!absent.hasSurface); QCOMPARE(absent.surfaceX, 0.0); QCOMPARE(absent.surfaceY, 0.0);
    QVERIFY(std::isnan(catalog.entityById(missingIds.front()).surfaceX));

    auto transformed=catalog.storedEntityById(ids.front()); transformed.coordinateStatus="ok";
    transformed.extra.insert("projectLon", 110.0); transformed.extra.insert("projectLat", 30.0);
    transformed.extra.insert("retained", "custom"); transformed.kb=42;
    QVERIFY(catalog.updateEntity(transformed, &error));
    const auto transformedSequence=catalog.mutationSeq();
    QVERIFY2(registerAreaWells(catalog, &error), qPrintable(error));
    QCOMPARE(catalog.mutationSeq(), transformedSequence);
    QCOMPARE(catalog.storedEntityById(ids.front()).extra.value("projectLon").toDouble(), 110.0);

    info.wellName="SYNTH-RENAMED"; info.wellheadX=300; info.wellheadY=400;
    info.totalDrilledDepth=2000; info.coordinateSystem="EPSG:32649"; info.coordinateUnit="ft";
    info.wellType="development"; info.fieldName="field"; info.remark="changed";
    QVERIFY2(model.saveWellInfo(modelId, info, detail), detail.c_str());
    QCOMPARE(catalog.entityById(ids.front()).name, QString("SYNTH-RENAMED"));
    QCOMPARE(catalog.entityById(ids.front()).surfaceX, 300.0); // Public overlay is already current.
    QCOMPARE(catalog.storedEntityById(ids.front()).surfaceX, 100.0);
    QVERIFY2(registerAreaWells(catalog, &error), qPrintable(error));
    QCOMPARE(catalog.mutationSeq(), transformedSequence+1);
    const auto updated=catalog.storedEntityById(ids.front());
    QCOMPARE(updated.name, QString("SYNTH-RENAMED")); QCOMPARE(updated.surfaceX, 300.0); QCOMPARE(updated.surfaceY, 400.0);
    QCOMPARE(updated.td, 2000.0); QCOMPARE(updated.kb, 42.0); QCOMPARE(updated.coordinateStatus, QString("untransformed"));
    QCOMPARE(updated.extra.value("wellCoordinatesCrs").toString(), QString("EPSG:32649"));
    QCOMPARE(updated.extra.value("wellCoordinateUnit").toString(), QString("ft"));
    QCOMPARE(updated.extra.value("wellType").toString(), QString("development"));
    QCOMPARE(updated.extra.value("fieldName").toString(), QString("field"));
    QCOMPARE(updated.extra.value("wellRemark").toString(), QString("changed"));
    QCOMPARE(updated.extra.value("retained").toString(), QString("custom"));
    QVERIFY(!updated.extra.contains("projectLon")); QVERIFY(!updated.extra.contains("projectLat"));
    const auto changedSequence=catalog.mutationSeq();
    QVERIFY2(registerAreaWells(catalog, &error), qPrintable(error)); QCOMPARE(catalog.mutationSeq(), changedSequence);

    info.wellheadX=paleo::data::Missing; info.wellheadY=paleo::data::Missing; info.totalDrilledDepth=paleo::data::Missing;
    QVERIFY2(model.saveWellInfo(modelId, info, detail), detail.c_str());
    QVERIFY(!catalog.entityById(ids.front()).hasSurface);
    QVERIFY2(registerAreaWells(catalog, &error), qPrintable(error));
    const auto normalized=catalog.storedEntityById(ids.front());
    QVERIFY(!normalized.hasSurface); QCOMPARE(normalized.surfaceX, 0.0); QCOMPARE(normalized.surfaceY, 0.0);
    QCOMPARE(normalized.td, 0.0); QCOMPARE(normalized.coordinateStatus, QString("missing"));
    const auto missingSequence=catalog.mutationSeq();
    QVERIFY2(registerAreaWells(catalog, &error), qPrintable(error)); QCOMPARE(catalog.mutationSeq(), missingSequence);
    DataCatalog reopened; QVERIFY2(reopened.open(project, &error), qPrintable(error));
    const auto persisted=reopened.storedEntityById(ids.front());
    QCOMPARE(persisted.name, QString("SYNTH-RENAMED")); QVERIFY(!persisted.hasSurface);
    QCOMPARE(persisted.surfaceX, 0.0); QCOMPARE(persisted.surfaceY, 0.0);
    QCOMPARE(persisted.extra.value("retained").toString(), QString("custom"));
  }
  void directoryModelDrivesBothPreviews() {
    QTemporaryDir temp;
    const QString input = temp.filePath("input"); QDir().mkpath(input);
    const QString source = input + "/well.xml";
    QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("<?xml version=\"1.0\"?><Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\" xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\"><Worksheet ss:Name=\"地层单位道\"><Table>"
        "<Row><Cell><Data ss:Type=\"String\">井号</Data></Cell><Cell><Data ss:Type=\"String\">顶深</Data></Cell><Cell><Data ss:Type=\"String\">底深</Data></Cell><Cell><Data ss:Type=\"String\">地层</Data></Cell></Row>"
        "<Row><Cell><Data ss:Type=\"String\">SYNTH-1</Data></Cell><Cell><Data ss:Type=\"Number\">100</Data></Cell><Cell><Data ss:Type=\"Number\">200</Data></Cell><Cell><Data ss:Type=\"String\">original</Data></Cell></Row>"
        "</Table></Worksheet></Workbook>"); file.close();
    const QString project = temp.filePath("project");
    paleo::data::ImportReport report; QString error;
    QVERIFY2(paleo::area_model::importDirectory(project, input, report, &error), qPrintable(error));
    QVERIFY(report.imported > 0 || report.review > 0);
    paleo::data::DataService model; std::string detail;
    QVERIFY(model.openProject(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    std::vector<paleo::data::DataEntry> entries;
    QVERIFY(model.listData("intervals", 0, entries, detail));
    QVERIFY2(!entries.empty(), detail.c_str());
    std::vector<paleo::data::IntervalRecord> rows;
    QVERIFY(model.readIntervals(entries.front().id, paleo::data::Missing, paleo::data::Missing, rows, detail));
    QCOMPARE(rows.size(), size_t(1));
    rows[0].value.intervalName = "edited-in-model";
    QVERIFY(model.saveInterval(entries[0].id, rows[0], detail));
    // Snapshot must include committed WAL, not just copy the main DB file.
    paleo::data::DataService snapshotReader;
    QVERIFY(snapshotReader.openProjectReadOnly(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    const QString snapshotRoot = temp.filePath("snapshot"); QDir().mkpath(snapshotRoot);
    QVERIFY2(snapshotReader.snapshotDatabase((snapshotRoot + "/data.sqlite").toUtf8().toStdString(), detail), detail.c_str());
    snapshotReader.closeProject(); model.closeProject();
    QVERIFY(snapshotReader.openProjectReadOnly(snapshotRoot.toUtf8().toStdString(), detail));
    QVERIFY(snapshotReader.readIntervals(entries[0].id, paleo::data::Missing, paleo::data::Missing, rows, detail));
    QCOMPARE(rows[0].value.intervalName, std::string("edited-in-model")); snapshotReader.closeProject();
    paleo::area_model::registerProject(project, this);
    PreviewDocService::WorkbookPreview table;
    WellComposite::ComprehensiveWellData chart; bool chartOk = false;
    QVERIFY(PreviewDocService::areaPreviewAt(source, &table, &chart, &chartOk, &error));
    QVERIFY2(table.ok, qPrintable(table.error)); QVERIFY2(chartOk, qPrintable(error));
    QCOMPARE(chart.formationIntervals.size(), 1);
    QCOMPARE(chart.formationIntervals[0].name, QString("edited-in-model"));
    QCOMPARE(table.sheets.size(), 2);
    QCOMPARE(table.sheets[0].name, QStringLiteral("井"));
    QCOMPARE(table.sheets[1].name, QStringLiteral("地震"));
    bool listed = false;
    for (const auto &row : table.sheets[0].rows)
      if (row.contains(QStringLiteral("SYNTH-1")) && row.contains(QStringLiteral("地层单位道"))) listed = true;
    QVERIFY(listed);
    // A source change cannot silently switch either view back to a file parser.
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); file.write("broken XML"); file.close();
    QVERIFY(PreviewDocService::wellCompositeAt(source, &chart, &error));
    QCOMPARE(chart.formationIntervals[0].name, QString("edited-in-model"));
    QVERIFY(PreviewDocService::xmlDataListAt(source).ok);
    paleo::area_model::unregisterProject(this);
    QVERIFY(!PreviewDocService::wellCompositeAt(source, &chart, &error));
  }
  void curveReadsUseTheAreaModel() {
    QTemporaryDir temp; const QString input = temp.filePath("input"); QDir().mkpath(input);
    const QString path = input + "/well.las"; QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\nNULL. -999.25\n~Curve\nDEPT.M : depth\nGR.API : gamma\n~Ascii\n100 10\n101 20\n"); file.close();
    const QString project = temp.filePath("project"); QString error; paleo::data::ImportReport report;
    QVERIFY2(paleo::area_model::importDirectory(project, input, report, &error), qPrintable(error));
    paleo::area_model::registerProject(project, this);
    paleo::data::DataService service; std::string detail;
    QVERIFY(service.openProject(paleo::area_model::directory(project).toUtf8().toStdString(), detail));
    std::vector<paleo::data::DataEntry> entries; QVERIFY(service.listData("curves", 0, entries, detail));
    QCOMPARE(entries.size(), size_t(1));
    QVERIFY(service.updateCurveValue(entries[0].id, 0, 88.0, detail)); service.closeProject();
    LasHeaderInfo header; QList<LasCurve> curves;
    QVERIFY2(WellLogRead::parse(path, header, curves, &error), qPrintable(error));
    QCOMPARE(curves[1].values[0], 88.0);
    // Incremental directory import must preserve the user's model edit.
    QVERIFY(paleo::area_model::importDirectory(project, input, report, &error));
    QVERIFY(WellLogRead::parse(path, header, curves, &error)); QCOMPARE(curves[1].values[0], 88.0);
    // A cached missing source must become visible after a DB/WAL update.
    const QString second = input + "/second.las";
    long long sourceId = 0; paleo::data::DataService reader;
    QCOMPARE(paleo::area_model::openSource(second, reader, sourceId, &error), paleo::area_model::Read::Unbound);
    QVERIFY(QFile::copy(path, second));
    QFile secondFile(second); QVERIFY(secondFile.open(QIODevice::Append));
    secondFile.write("# independent source\n"); secondFile.close();
    QVERIFY(paleo::area_model::importDirectory(project, input, report, &error));
    QCOMPARE(paleo::area_model::openSource(second, reader, sourceId, &error), paleo::area_model::Read::Ready);
    QVERIFY(sourceId > 0);
    QVERIFY(reader.listSourceData("curves", sourceId, entries, detail));
    QCOMPARE(entries.size(), size_t(1)); QCOMPARE(entries[0].sourceId, sourceId);
    QVERIFY(reader.listSourceData("tables", sourceId, entries, detail));
    for (const auto &entry : entries) QCOMPARE(entry.sourceId, sourceId);
    QVERIFY(reader.listSourceData("curves", sourceId + 999999, entries, detail)); QVERIFY(entries.empty());
    QVERIFY(!reader.listSourceData("curves", 0, entries, detail));
    paleo::area_model::unregisterProject(this);
  }
  void excludedAndCancelledDirectories() {
    QTemporaryDir temp; QString error; paleo::data::ImportReport report;
    const QString source = temp.filePath("source"); QDir().mkpath(source + "/output");
    QFile f(source + "/output/data.txt"); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("ignore"); f.close();
    QVERIFY(paleo::area_model::importDirectory(temp.filePath("project"), source, report, &error, {}, {source + "/output"}));
    QCOMPARE(report.seen, 0);
    QVERIFY(!paleo::area_model::importDirectory(temp.filePath("cancelled"), source, report, &error, [] { return true; }));
    QVERIFY(report.cancelled);
  }
};
QTEST_GUILESS_MAIN(TestAreaModel)
#include "tst_areamodel.moc"
