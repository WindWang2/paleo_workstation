#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "io/sourcefingerprint.h"
#include "services/documentsourceprobe.h"
#include "services/documentrelationresolver.h"
#include "services/areawellchart.h"
#include "services/projectdata.h"
#include "services/seismicdataaccess.h"
#include "services/stratigraphicsubintervals.h"
#include "workflow/sectionworkbench.h"
#include "workflow/wellsectionworkflow.h"
#include "domain/wellcompositemodel.h"
#include <paleo_data/object_descriptor.h>
#include <paleo_data/parser.h>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <cmath>

using namespace document_fixture;
class TestDocumentSourcesHy:public QObject {
    Q_OBJECT
    QString root;
    using Inventory=QMap<QString,QPair<qint64,QDateTime>>;
    Inventory before;
    QStringList files(const QString &directory,const QStringList &filters={"*"}) const {
        QStringList result;QDirIterator iterator(QDir(root).filePath(directory),filters,QDir::Files,QDirIterator::Subdirectories);
        while(iterator.hasNext())result.append(iterator.next());result.sort();return result;
    }
    Inventory inventory() const {
        Inventory result;
        QDirIterator iterator(root,QDir::Files|QDir::Hidden|QDir::System,QDirIterator::Subdirectories);
        while(iterator.hasNext()) {const auto path=iterator.next();const QFileInfo file(path);result.insert(path,{file.size(),file.lastModified()});}
        return result;
    }
    static QString reference(Fixture &fixture,const QString &path,const QString &digest={}) {
        CatalogAsset asset;asset.id=fixture.catalog.nextAssetId();asset.type="document";asset.displayName=QFileInfo(path).fileName();
        if(!fixture.catalog.addAsset(asset,&fixture.error))return {};
        CatalogVersion version;version.id=fixture.catalog.nextVersionId();version.assetId=asset.id;version.stage="RAW";
        version.managed=false;version.path=path;version.sourceUri=path;version.fileName=QFileInfo(path).fileName();version.sha256=digest;
        return fixture.catalog.addVersion(version,&fixture.error)?version.id:QString();
    }
    static SourceCommandResult invoke(Fixture &fixture,SourceCommand command) {
        QSignalSpy finished(&fixture.session,&DocumentSession::commandFinished);
        auto result=fixture.session.execute(command);
        if(!result.pending)return result;
        if(finished.isEmpty() && !finished.wait(15000)) {
            result.error="HY source operation timed out";
            return result;
        }
        return qvariant_cast<SourceCommandResult>(finished.first().first());
    }
    static bool importWellCheckshots(const QString &copy,const QString &wellName,
                                     paleo::data::DataService &service,std::string &error) {
        const auto path=copy.toUtf8().toStdString();
        const auto canonical=paleo::data::canonicalWellName(wellName.toStdString());
        const auto parsed=paleo::data::parseFile(path);
        paleo::data::ParsedFile selected;
        selected.kind=parsed.kind;selected.status="parsed";selected.coordinateSystems=parsed.coordinateSystems;
        for(const auto &well:parsed.wells)
            if(paleo::data::canonicalWellName(well.wellInfo.wellName)==canonical)selected.wells.push_back(well);
        if(selected.wells.size()!=1 || selected.wells.front().timeDepthRelations.size()!=1) {
            error="HY fixture needs exactly one assigned relation for the selected well";return false;
        }
        QFile file(copy);
        QCryptographicHash digest(QCryptographicHash::Sha256);
        if(!file.open(QIODevice::ReadOnly) || !digest.addData(&file)) {
            error=file.errorString().toStdString();return false;
        }
        file.close();
        long long source=0;
        return service.importPreparedFile(path,path,path,digest.result().toHex().toStdString(),selected,source,error);
    }
private slots:
    void initTestCase() {
        root=qEnvironmentVariable("PALEO_HY_SOURCE_ROOT");
        if(root.isEmpty())QSKIP("Set PALEO_HY_SOURCE_ROOT for read-only external-data validation.");
        QVERIFY(QFileInfo(root).isDir());before=inventory();QVERIFY(!before.isEmpty());
        qint64 bytes=0;for(const auto &entry:before)bytes+=entry.first;
        qInfo("HY input inventory files=%lld bytes=%lld",qint64(before.size()),bytes);
    }
    void sameWellHasMultipleIndependentCheckshotSources() {
        Fixture f;QVERIFY(f.init());const auto sources=files(QStringLiteral("时深转换"));QVERIFY(sources.size()>=2);
        for(const auto &path:sources)QVERIFY2(f.importModel(path),qPrintable(f.error));
        QVERIFY(f.bind());int multiple=0;
        for(const auto &well:f.catalog.entities("well")) {
            auto bindings=f.session.bindings("well",well.id,"time_depth");if(bindings.size()<2)continue;
            ++multiple;QVERIFY(bindings[0].source.key()!=bindings[1].source.key());
            auto command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId=bindings[1].id;
            const auto result=f.session.execute(command);QVERIFY2(result.ok,qPrintable(result.error));
            bindings=f.session.bindings("well",well.id,"time_depth");
            QCOMPARE(std::count_if(bindings.begin(),bindings.end(),[](const auto &value){return value.primary;}),1);
        }
        QVERIFY(multiple>0);qInfo("HY checkshot sources=%lld, wells with multiple sources=%d",qint64(sources.size()),multiple);
    }
    void typedRelationsKeepActualObjectsIndependentByPurpose() {
        Fixture f;QVERIFY(f.init());const auto paths=files(QStringLiteral("时深转换"));QVERIFY(paths.size()>=2);
        for(const auto &path:paths)QVERIFY2(f.importModel(path),qPrintable(f.error));QVERIFY(f.bind());
        const auto records=f.session.sources();paleo::data::DataService service;std::string error;
        QVERIFY(service.openProjectReadOnly(paleo::area_model::directory(f.project).toStdString(),error));
        const auto invoke=[&](SourceCommand command) {
            QSignalSpy finished(&f.session,&DocumentSession::commandFinished);auto result=f.session.execute(command);
            if(result.pending) {
                if(finished.isEmpty())finished.wait(10000);
                if(finished.isEmpty()){result.ok=false;result.error="HY typed relation timed out";return result;}
                result=qvariant_cast<SourceCommandResult>(finished.front().front());
            }
            return result;
        };
        bool verified=false;
        for(const auto &projected:f.catalog.entities("well")) {
            const auto well=f.catalog.storedEntityById(projected.id);const auto modelWell=well.extra.value("areaModelWellId").toLongLong();
            if(modelWell<=0)continue;
            std::vector<paleo::data::DataEntry> entries;QVERIFY(service.listData("time_depth",modelWell,entries,error));
            if(entries.size()<2)continue;
            const auto second=std::find_if(entries.begin()+1,entries.end(),[&](const auto &entry){return entry.sourceId!=entries.front().sourceId;});
            if(second==entries.end())continue;
            QVector<RelationUseSpec> uses;QVector<ModelObjectRef> objects;
            for(const auto &entry:{entries.front(),*second}) {
                const auto record=std::find_if(records.begin(),records.end(),[&](const auto &value){return value.ref.sourceId==entry.sourceId;});
                QVERIFY(record!=records.end());paleo::data::DataObjectDescriptor descriptor;
                QVERIFY(service.describeDataObject(paleo::data::DataObjectKind::TimeDepth,entry.id,descriptor,error));QVERIFY(descriptor.revisionKnown);
                RelationUseSpec use;use.context.documentId=f.session.documentId();use.context.modelStoreId=record->ref.modelStoreId;
                use.context.purpose=uses.isEmpty()?RelationPurpose::Correlation:RelationPurpose::WellTie;
                use.wellEntityId=well.id;use.modelWellId=modelWell;
                ModelObjectRef object{record->ref,ObjectKind::TimeDepth,entry.id,QString::fromStdString(descriptor.revision)};
                SourceBinding binding;binding.id="hy-typed-"+QString::number(entry.id);QVERIFY(setRelationBinding(binding,use,object,&f.error));
                auto command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=binding;
                auto result=invoke(command);QVERIFY2(result.ok,qPrintable(result.error));
                for(const auto kind:{SourceCommand::Kind::ConfirmBinding,SourceCommand::Kind::SelectPrimary}) {
                    command=f.session.command(kind);command.bindingId=binding.id;result=invoke(command);QVERIFY2(result.ok,qPrintable(result.error));
                }
                uses.append(use);objects.append(object);
            }
            for(int i=0;i<uses.size();++i) {
                const auto view=f.session.relationView(uses[i]);QVERIFY(view.selected);QCOMPARE(view.selectedObject,objects[i]);
                QString detail;const auto request=f.session.relationRequest(uses[i],&detail);QVERIFY2(detail.isEmpty(),qPrintable(detail));
                const auto resolved=resolveDocumentRelation(request);QVERIFY2(resolved.ok,qPrintable(resolved.error));QCOMPARE(resolved.object,objects[i]);
            }
            QVERIFY(objects[0].source!=objects[1].source);QVERIFY(objects[0].id!=objects[1].id);
            qInfo("HY typed uses isolated by actual objects=%lld/%lld, sources=%lld/%lld",objects[0].id,objects[1].id,objects[0].source.sourceId,objects[1].source.sourceId);
            verified=true;break;
        }
        QVERIFY(verified);
    }
    void worksheetIdentityConflictRemainsUnconfirmed() {
        const QRegularExpression namePattern(QStringLiteral("(?:HZ|XJ)[0-9]+-[0-9]+-[A-Za-z0-9]+"));
        QString path,claimed,fileNameClaim;
        for(const auto &candidate:files(QStringLiteral("岩心"),{"*.xls"})) {
            if(!QFileInfo(candidate).fileName().contains(QStringLiteral("伽马曲线图")))continue;
            const auto model=paleo::data::parseFile(candidate.toUtf8().toStdString());
            const auto name=namePattern.match(QFileInfo(candidate).fileName()).captured();
            for(const auto &sheet:model.sourceTables)for(const auto &cell:sheet.cells) {
                const auto text=QString::fromUtf8(cell.text);
                const auto internal=namePattern.match(text).captured();
                if(internal.isEmpty() || name.isEmpty() || internal.compare(name,Qt::CaseInsensitive)==0)continue;
                const bool declared=std::any_of(sheet.cells.begin(),sheet.cells.end(),[&](const auto &label){
                    auto value=QString::fromUtf8(label.text);value.remove(QRegularExpression("\\s"));
                    return label.row==cell.row && std::abs(label.column-cell.column)<=2 && value.contains(QStringLiteral("井号"));
                });
                if(declared) {path=candidate;claimed=internal;fileNameClaim=name;break;}
            }
            if(!path.isEmpty())break;
        }
        QVERIFY2(!path.isEmpty(),"Expected at least one explicit filename/worksheet identity conflict in the supplied dataset");
        Fixture f;QVERIFY(f.init());const auto version=reference(f,path);QVERIFY(!version.isEmpty());
        CatalogEntity well;well.id="review-well";well.entityType="well";well.name=fileNameClaim;QVERIFY(f.catalog.addEntity(well));QVERIFY(f.bind());
        auto command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding.id="hy-conflict";
        command.binding.resourceKind="well";command.binding.resourceId=well.id;command.binding.role="core";
        command.binding.source=f.session.sourceForVersion(version).ref;
        command.binding.identityClaims={{"filename",fileNameClaim},{"worksheet",claimed}};
        QVERIFY(f.session.execute(command).ok);const auto bindings=f.session.bindings("well",well.id);
        QCOMPARE(bindings.size(),1);QCOMPARE(bindings.front().decision,BindingDecision::Conflict);QVERIFY(!bindings.front().primary);
        qInfo().noquote()<<"HY identity conflict retained:"<<fileNameClaim<<claimed;
    }
    void selectedWellSeismicInterpretationReadsBoundedWindowsAfterRelocation() {
        const QString wellName="HZ19-6-1D";
        Fixture f;QVERIFY(f.init());
        const auto logs=files(QStringLiteral("单井数据"),{wellName+".xml"});
        const auto checkshots=files(QStringLiteral("时深转换"));
        const auto seismicFiles=files(QStringLiteral("地震数据"),{"*.sgy"});
        QCOMPARE(logs.size(),1);QVERIFY(checkshots.size()>=2);QVERIFY(!seismicFiles.isEmpty());
        // Match the comprehensive-well import contract used by the application.
        QVERIFY2(f.importModel(logs.front(),true),qPrintable(f.error));
        QVERIFY2(f.importModel(seismicFiles.front()),qPrintable(f.error));
        paleo::data::DataService service;std::string detail;
        QVERIFY2(service.openProject(paleo::area_model::directory(f.project).toStdString(),detail),detail.c_str());
        // The other HY cases cover unresolved and conflicting wells. Here only
        // the explicitly identified well's original samples enter the fixture.
        for(const auto &path:checkshots) {
            const auto copy=f.temp.filePath(QFileInfo(path).fileName());
            QVERIFY(QFile::copy(path,copy));
            QVERIFY2(importWellCheckshots(copy,wellName,service,detail),detail.c_str());
        }
        QVERIFY2(registerAreaWells(f.catalog,&f.error),qPrintable(f.error));
        QVERIFY2(registerAreaSources(f.catalog,&f.error),qPrintable(f.error));
        QVERIFY(f.bind());
        const auto ids=f.catalog.wellsMatchingName(wellName);
        QCOMPARE(ids.size(),1);
        const auto well=f.catalog.storedEntityById(ids.front());
        const auto modelWell=well.extra.value("areaModelWellId").toLongLong();
        QVERIFY(modelWell>0);
        std::vector<paleo::data::DataEntry> tables,curves,volumes;
        QVERIFY(service.listData("time_depth",modelWell,tables,detail));QCOMPARE(tables.size(),size_t(2));
        QVERIFY(service.listData("curves",modelWell,curves,detail));QVERIFY(!curves.empty());
        QVERIFY(service.listData("volumes",0,volumes,detail));QVERIFY(!volumes.empty());

        const auto log=std::find_if(curves.begin(),curves.end(),[](const auto &entry){return entry.name=="GR";});
        QVERIFY(log!=curves.end());
        paleo::data::CurveWindowQuery query;query.curveId=log->id;query.firstSample=log->count/2;query.sampleCount=4;
        paleo::data::CurveWindowResult samples;paleo::data::CurveReadStats bytes;
        QVERIFY2(service.readCurveWindow(query,samples,detail,{},&bytes),detail.c_str());
        QVERIFY(bytes.chunked);QCOMPARE(bytes.legacyBytesMaterialized,0LL);QCOMPARE(samples.curve.depths.size(),size_t(4));
        QVERIFY(log->count>10000);QVERIFY(bytes.valueBlocksRead>=1 && bytes.valueBlocksRead<=2);
        const auto &depths=samples.curve.depths;
        QVERIFY(std::isfinite(depths.front()) && depths.back()>depths.front());
        const double scale=samples.depthInfo.depthUnit=="ft"?0.3048:1.;
        AreaWellCurveWindow window;window.topMd=depths.front()*scale;window.bottomMd=depths.back()*scale;
        window.maxSamples=32;window.maxReadBytes=1024*1024;
        QVector<wellsection::Curve> resultCurves;
        QVERIFY2(readAreaWellSectionCurves(f.project,modelWell,{"GR"},resultCurves,&f.error,{},window),qPrintable(f.error));
        QCOMPARE(resultCurves.size(),1);QCOMPARE(resultCurves.front().depths.size(),qsizetype(4));
        for(qsizetype i=0;i<4;++i)
            QCOMPARE(resultCurves.front().values[i],float(samples.curve.values[i]));

        const auto draft=paleo::strat::draftScheme("hy-test","HY test-only revision",{"TOP","BASE"},
            {{modelWell,{{"TOP",window.topMd},{"BASE",window.bottomMd}}}});
        paleo::data::StratigraphicScheme scheme;std::vector<paleo::data::FrameworkIssue> issues;
        QVERIFY2(service.commitStratigraphicScheme(draft,{},scheme,issues,detail),detail.c_str());
        const auto sourceFor=[&](long long id) {
            for(const auto &record:f.session.sources())if(record.ref.sourceId==id)return record;
            return SourceRecord{};
        };
        const auto volumeSource=sourceFor(volumes.front().sourceId);
        const auto tableSource=sourceFor(tables.back().sourceId);
        QVERIFY(volumeSource.ref.valid() && tableSource.ref.valid());
        // Explicit fixture conventions in the private model. The original files
        // do not declare these datums and remain unchanged.
        QVERIFY(service.confirmTimeDepthAlignment({tables.back().id},"ms","TWT","HY-test-TWT",true,volumes.front().id,detail));
        paleo::data::DataObjectDescriptor unconfirmed,table,volume;
        QVERIFY(service.describeDataObject(paleo::data::DataObjectKind::TimeDepth,tables.back().id,unconfirmed,detail));
        QVERIFY2(service.confirmTimeDepthSemanticsAtRevision(unconfirmed.id,unconfirmed.revision,
            "HY-test-MD","ms","TWT","HY-test-TWT",true,table,detail),detail.c_str());
        QVERIFY(service.describeDataObject(paleo::data::DataObjectKind::SeismicVolume,volumes.front().id,volume,detail));
        InterpretationContext context;
        context.documentId=f.session.documentId();context.modelStoreId="area-model";context.purpose=RelationPurpose::WellTie;
        context.schemeId=QString::fromStdString(scheme.id);context.schemeRevision=QString::fromStdString(scheme.revision.value);
        context.targetVolume=ModelObjectRef{volumeSource.ref,ObjectKind::SeismicVolume,volume.id,QString::fromStdString(volume.revision)};
        QVERIFY2(f.session.selectInterpretation(context,&f.error),qPrintable(f.error));
        RelationUseSpec use{context,well.id,modelWell,RelationKind::TimeDepth};
        const ModelObjectRef tableRef{tableSource.ref,ObjectKind::TimeDepth,table.id,QString::fromStdString(table.revision)};
        SourceBinding binding;binding.id="hy-selected-relation";QVERIFY(setRelationBinding(binding,use,tableRef,&f.error));
        auto command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=binding;
        const auto registered=invoke(f,command);QVERIFY2(registered.ok,qPrintable(registered.error));
        for(const auto kind:{SourceCommand::Kind::ConfirmBinding,SourceCommand::Kind::SelectPrimary}) {
            command=f.session.command(kind);command.bindingId=binding.id;
            const auto selected=invoke(f,command);QVERIFY2(selected.ok,qPrintable(selected.error));
        }
        ProjectDataFacade facade;facade.setCatalog(&f.catalog,f.project);
        const auto selected=f.session.interpretationRelation(well.id,RelationKind::TimeDepth);
        QCOMPARE(selected.state,RelationInput::State::Selected);QCOMPARE(selected.request.object,tableRef);
        const auto values=facade.tdTableForSelection(well.id,selected);
        QVERIFY2(values.size()>100,qPrintable(facade.lastError()));
        const auto tops=facade.pinnedMappingTops(context,{"TOP","BASE"},&f.error).value(well.id);
        QVERIFY2(f.error.isEmpty(),qPrintable(f.error));
        const auto mappingTop=std::find_if(tops.begin(),tops.end(),[](const auto &top){return top.horizon=="TOP";});
        QVERIFY(mappingTop!=tops.end());QCOMPARE(mappingTop->md,window.topMd);
        AreaWellChartQuery chartQuery;
        chartQuery.curves=window;chartQuery.curveNames=QStringList{"GR"};chartQuery.context=context;
        chartQuery.context->purpose=RelationPurpose::WellColumn;
        WellComposite::ComprehensiveWellData chart;
        QVERIFY2(readAreaWellChart(service,modelWell,chart,&f.error,{},&chartQuery),qPrintable(f.error));
        QVERIFY(!chart.formationIntervals.isEmpty());
        QCOMPARE(chart.formationIntervals.front().topDepth,float(window.topMd));
        SectionWorkbench section(&f.catalog);section.setDocumentSession(&f.session);
        seismic::TimeDepthModel model;double shift=0;QString status;
        QVERIFY2(section.mdTimeDepth(well.id,&model,&shift,&status),qPrintable(status));
        WellSectionWorkflow sectionWorkflow(&f.catalog);
        sectionWorkflow.setSectionWorkbench(&section);
        sectionWorkflow.setDocumentSession(&f.session);
        QSignalSpy drawn(&sectionWorkflow,&WellSectionWorkflow::sectionReady);
        sectionWorkflow.request({well.id},{"GR"});
        QTRY_COMPARE(drawn.size(),1);
        const auto sectionWells=drawn.front().at(1).value<QVector<wellsection::Well>>();
        QCOMPARE(sectionWells.size(),1);QVERIFY(sectionWells.front().timeDepth);
        QCOMPARE(sectionWells.front().correlationRevision,context.schemeId+":"+context.schemeRevision);

        const auto moved=f.temp.filePath("moved-checkshot");
        QVERIFY(QFile::rename(tableSource.path,moved));
        command=f.session.command(SourceCommand::Kind::Relocate);command.source=tableSource.ref;command.path=moved;
        const auto relocated=invoke(f,command);QVERIFY2(relocated.ok,qPrintable(relocated.error));
        const auto movedSelection=f.session.interpretationRelation(well.id,RelationKind::TimeDepth);
        QCOMPARE(movedSelection.request.object,tableRef);
        QCOMPARE(movedSelection.request.selected.path,moved);
        const auto after=facade.tdTableForSelection(well.id,movedSelection);
        QCOMPARE(after.size(),values.size());
        for(qsizetype i=0;i<values.size();++i) {
            QCOMPARE(after[i].md,values[i].md);
            QCOMPARE(after[i].timeMs,values[i].timeMs);
        }
        seismic::TimeDepthModel movedModel;double movedShift=0;
        QVERIFY2(section.mdTimeDepth(well.id,&movedModel,&movedShift,&status),qPrintable(status));
        QCOMPARE(movedModel.points().size(),model.points().size());
        QCOMPARE(movedModel.points().back().timeMs,model.points().back().timeMs);
        QCOMPARE(movedShift,shift);

        paleo::SeismicDataAccess seismic;
        QVERIFY2(seismic.openModelVolume(volumeSource,*context.targetVolume,&f.error),qPrintable(f.error));
        paleo::data::SeismicFileInfo info;
        QVERIFY2(seismic.inspect(info,detail),detail.c_str());
        paleo::data::SectionReadOptions options;
        options.firstTime=info.firstTime;options.lastTime=info.firstTime+2*info.sampleInterval;
        options.maxColumns=8;options.maxValues=24;
        paleo::data::SeismicSectionResult slice;
        QElapsedTimer elapsed;elapsed.start();
        QVERIFY2(seismic.line(true,info.inlineMin,options,slice,detail),detail.c_str());
        QVERIFY(slice.columns>0 && slice.columns<=8 && slice.samples>0 && slice.samples<=3);
        QVERIFY(std::any_of(slice.amplitudes.cbegin(),slice.amplitudes.cend(),[](float v){return std::isfinite(v);}));
        qInfo()<<"HY selected samples"<<values.size()<<"curve SQL bytes"<<bytes.payloadBytesRead
               <<"seismic window"<<slice.columns<<slice.samples<<"elapsed ms"<<elapsed.elapsed();
    }
    void relocationUsesOnlyAPrivateCopyOfRealLogData() {
        auto logs=files(QStringLiteral("岩心"),{"*.las"});QVERIFY(!logs.isEmpty());
        logs.removeIf([](const auto &path){return QFileInfo(path).size()==0;});QVERIFY(!logs.isEmpty());
        std::sort(logs.begin(),logs.end(),[](const auto &a,const auto &b){return QFileInfo(a).size()<QFileInfo(b).size();});
        QFile original(logs.front());QVERIFY(original.open(QIODevice::ReadOnly));const auto bytes=original.readAll();original.close();
        const auto digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
        Fixture f;QVERIFY(f.init());const auto copy=f.temp.filePath("private.las");QVERIFY(write(copy,bytes));QVERIFY2(f.importModel(copy),qPrintable(f.error));QVERIFY(f.bind());
        {
            paleo::data::DataService reader;std::string detail;
            QVERIFY(reader.openProjectReadOnly(paleo::area_model::directory(f.project).toStdString(),detail));
            std::vector<paleo::data::DataEntry> curves;QVERIFY(reader.listData("curves",0,curves,detail));QVERIFY(!curves.empty());
            paleo::data::CurveWindowQuery query;query.curveId=curves.front().id;query.sampleCount=1;
            paleo::data::CurveWindowResult window;paleo::data::CurveReadStats usage;
            QVERIFY2(reader.readCurveWindow(query,window,detail,{},&usage),detail.c_str());
            QVERIFY(usage.chunked);QCOMPARE(usage.legacyBytesMaterialized,0LL);QCOMPARE(window.curve.values.size(),size_t(1));
            qInfo("HY curve first sample: SQL payload=%lld axis blocks=%lld value blocks=%lld",usage.payloadBytesRead,usage.axisBlocksRead,usage.valueBlocksRead);
        }
        const auto record=f.session.sources().front();const auto moved=f.temp.filePath("relocated/private.las");
        QVERIFY(QDir().mkpath(QFileInfo(moved).absolutePath()));QVERIFY(QFile::rename(copy,moved));
        auto command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=moved;
        QSignalSpy finished(&f.session,&DocumentSession::commandFinished);QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(finished.size(),1);
        const auto result=qvariant_cast<SourceCommandResult>(finished.front().front());QVERIFY2(result.ok,qPrintable(result.error));
        QCOMPARE(f.catalog.currentVersion(record.assetId).path,moved);
        QVERIFY(original.open(QIODevice::ReadOnly));QCOMPARE(QCryptographicHash::hash(original.readAll(),QCryptographicHash::Sha256),digest);
        qInfo("HY private-copy relocation verified bytes=%lld",result.state.bytesRead);
    }
    void seismicAndIncompletePagedChecksStayBounded() {
        Fixture f;QVERIFY(f.init());int complete=0,incomplete=0;QElapsedTimer timer;timer.start();qint64 totalRead=0;
        for(const auto &path:files(QStringLiteral("地震数据"),{"*.sf3p","*.sf3p.partial"})) {
            const auto version=reference(f,path);QVERIFY(!version.isEmpty());
        }
        QVERIFY(f.bind());QSignalSpy observed(&f.session,&DocumentSession::sourceObserved);
        for(const auto &record:f.session.sources()) {
            auto command=f.session.command(SourceCommand::Kind::Inspect);command.source=record.ref;
            const int previous=observed.size();QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(observed.size(),previous+1);
            const auto state=qvariant_cast<SourceState>(observed.back().at(1));
            QCOMPARE(state.file,FileAvailability::Present);QVERIFY(state.bytesRead<=1024*1024+32);QVERIFY(!state.payloadVerified);
            totalRead+=state.bytesRead;complete+=state.derivative==DerivativeState::HeaderComplete;incomplete+=state.derivative==DerivativeState::Incomplete;
        }
        QVERIFY(complete>0);QVERIFY(incomplete>0);
        const auto raw=files(QStringLiteral("地震数据"),{"*.sgy"});QVERIFY(!raw.isEmpty());QFile input(raw.front());QVERIFY(input.open(QIODevice::ReadOnly));
        const auto prefix=input.read(65536);QCryptographicHash hash(QCryptographicHash::Sha256);hash.addData(prefix);
        hash.addData(QByteArray::number(input.size()));hash.addData(QByteArray::number(QFileInfo(input).lastModified().toMSecsSinceEpoch()));input.close();
        const auto expected=SourceFingerprint::fromStored("quick:"+QString::fromLatin1(hash.result().toHex()));FileStamp stamp;qint64 bytes=0;QString error;
        QVERIFY2(verifySourceFingerprint(raw.front(),expected,stamp,bytes,&error),qPrintable(error));QVERIFY(bytes<=65536);
        qInfo("HY paged complete=%d incomplete=%d header bytes=%lld, SEG-Y verify bytes=%lld, total ms=%lld",complete,incomplete,totalRead,bytes,timer.elapsed());
    }
    void cleanupTestCase() {if(!root.isEmpty())QCOMPARE(inventory(),before);}
};
QTEST_GUILESS_MAIN(TestDocumentSourcesHy)
#include "tst_document_sources_hy.moc"
