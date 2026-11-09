#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "domain/documentrelation.h"
#include "services/projectdata.h"
#include "workflow/mappingworkflow.h"
#include "workflow/sectionworkbench.h"
#include "workflow/wellsectionworkflow.h"
#include "workflow/xmlpreviewsession.h"
#include <paleo_data/object_descriptor.h>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

using namespace document_fixture;
class TestDocumentRelationReader:public QObject {
    Q_OBJECT
    static SourceCommandResult invoke(Fixture &fixture,SourceCommand command) {
        QSignalSpy finished(&fixture.session,&DocumentSession::commandFinished);
        auto result=fixture.session.execute(command);
        if(!result.pending)return result;
        if(finished.isEmpty() && !finished.wait(10000)) {
            result.pending=false;result.error="relation command timed out";return result;
        }
        return qvariant_cast<SourceCommandResult>(finished.first().first());
    }
    static bool setMdReference(const QString &root) {
        const auto name="typed-reader-"+QUuid::createUuid().toString();bool ok=false;
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(root+"/data.sqlite");
            if(db.open()){QSqlQuery query(db);ok=query.exec("UPDATE well_objects SET payload=json_set(payload,'$.depth.md','KB') WHERE kind='time_depth'");}
        }
        QSqlDatabase::removeDatabase(name);return ok;
    }
private slots:
    void chosenContextFeedsProductionViews() {
        Fixture f;QVERIFY(f.init());
        QVERIFY(f.preparedWellSource("synthetic-same-source.txt",{"A"},{"checks","checks"},{"地层系统甲","地层系统乙"},true));
        QVERIFY(f.bind());
        const auto well=f.catalog.storedEntities("well").front();
        const auto modelWell=well.extra.value("areaModelWellId").toLongLong();
        QVERIFY(modelWell>0);
        paleo::data::DataService service;std::string error;
        QVERIFY(service.openProject(paleo::area_model::directory(f.project).toStdString(),error));
        paleo::data::StratigraphicScheme definition;definition.id="test-scheme";definition.name="Test scheme";
        for(const auto &name:{"TOP","BASE"}) {
            paleo::data::StratigraphicBoundary boundary;boundary.name=name;boundary.order=int(definition.boundaries.size())+1;
            definition.boundaries.push_back(boundary);
        }
        paleo::data::StratigraphicScheme first,pinned,head;
        std::vector<paleo::data::FrameworkIssue> issues;
        QVERIFY2(service.commitStratigraphicScheme(definition,{},first,issues,error),error.c_str());
        QVERIFY(!first.intervals.empty());
        paleo::data::WellIntervalOccurrence occurrence;occurrence.wellId=modelWell;occurrence.interval=first.intervals.front().id;
        occurrence.top=120.;occurrence.bottom=180.;
        first.wellOccurrences.push_back(occurrence);
        QVERIFY2(service.commitStratigraphicScheme(first,first.revision.value,pinned,issues,error),error.c_str());
        first=pinned;first.wellOccurrences.front().bottom=160.;
        QVERIFY2(service.commitStratigraphicScheme(first,pinned.revision.value,head,issues,error),error.c_str());
        InterpretationContext context;
        context.documentId=f.session.documentId();context.modelStoreId="area-model";context.purpose=RelationPurpose::WellTie;
        context.schemeId=QString::fromStdString(pinned.id);context.schemeRevision=QString::fromStdString(pinned.revision.value);
        QVERIFY2(f.session.selectInterpretation(context,&f.error),qPrintable(f.error));
        QCOMPARE(f.session.interpretationRelation(well.id,RelationKind::TimeDepth).state,RelationInput::State::Invalid);
        QVERIFY(f.session.clearInterpretation(&f.error));
        QCOMPARE(f.session.interpretationRelation(well.id,RelationKind::TimeDepth).state,RelationInput::State::Unselected);
        const auto choices=f.session.interpretationRelationOptions(well.id,&f.error);
        QCOMPARE(choices.size(),4);
        ModelObjectRef td,track;
        QVERIFY(modelObjectRefFromJson(QJsonObject::fromVariantMap(choices[1].toMap().value("object").toMap()),td));
        QVERIFY(modelObjectRefFromJson(QJsonObject::fromVariantMap(choices[3].toMap().value("object").toMap()),track));
        QVERIFY(td.kind==ObjectKind::TimeDepth);QVERIFY(track.kind==ObjectKind::IntervalTrack);
        auto command=f.session.chooseRelationCommand(context,well.id,td,
            {{"mdReference","KB"},{"unit","s"},{"type","TWT"},{"reference","fixture"},{"negate",false}},&f.error);
        auto done=invoke(f,command);QVERIFY2(done.ok,qPrintable(done.error));
        command=f.session.chooseRelationCommand(context,well.id,track,{},&f.error);
        done=invoke(f,command);QVERIFY2(done.ok,qPrintable(done.error));
        f.session.close();QVERIFY(f.bind(true));
        QVERIFY(f.session.interpretationSelection().context==context);
        XmlPreviewSession chart;
        chart.setTaskService(&f.tasks);chart.setDocumentSession(&f.session,well.id);
        QSignalSpy chartReady(&chart,&XmlPreviewSession::ready);
        chart.openModelWell(f.project,modelWell,well.name);
        QTRY_COMPARE_WITH_TIMEOUT(chartReady.size(),1,10000);
        const auto shown=qvariant_cast<XmlPreviewData>(chartReady.first().first());
        QVERIFY2(shown.chartOk,qPrintable(shown.chartError));
        QCOMPARE(shown.chart.stratigraphyIntervals.size(),1);
        QCOMPARE(shown.chart.stratigraphyIntervals.front().topDepth,110.f);
        QVERIFY(shown.interpretationSummary.contains(context.schemeRevision));
        SectionWorkbench alignment(&f.catalog);alignment.setDocumentSession(&f.session);
        WellSectionWorkflow section(&f.catalog);section.setDocumentSession(&f.session);section.setSectionWorkbench(&alignment);
        QVector<wellsection::Well> sectionWells;
        connect(&section,&WellSectionWorkflow::sectionReady,this,[&](int,const auto &wells,const auto &){sectionWells=wells;});
        section.request({well.id},{});
        QCOMPARE(sectionWells.size(),1);QVERIFY(sectionWells.front().timeDepth);
        QCOMPARE(sectionWells.front().timeDepth->twtAt(100.),220000.);
        ProjectDataFacade facade;facade.setCatalog(&f.catalog,f.project);
        MappingWorkflow mapping(nullptr,nullptr,nullptr);mapping.setProjectData(&facade);mapping.setDocumentSession(&f.session);
        const auto samples=mapping.computeThicknessSamples("TOP","BASE",&f.error);
        QVERIFY2(f.error.isEmpty(),qPrintable(f.error));QCOMPARE(samples.size(),1);
        QCOMPARE(samples.front().thickness,60.);QCOMPARE(samples.front().dtMs,60000.);
        QVERIFY(service.confirmTimeDepthSemantics(td.id,"ms","TWT","changed-datum",false,error));
        section.request({well.id},{});
        QVERIFY(!sectionWells.front().timeDepth);
        QVERIFY(!sectionWells.front().timeDepthStatus.isEmpty());
        const auto stale=mapping.computeThicknessSamples("TOP","BASE",&f.error);
        QCOMPARE(stale.size(),1);QVERIFY(!std::isfinite(stale.front().dtMs));
        QVERIFY(!stale.front().reason.isEmpty());
    }
    void exactRelationFeedsFacadeAndOldRevisionNeverFallsBack_data() {
        QTest::addColumn<QString>("relocation");
        QTest::newRow("document-command")<<QStringLiteral("session");
        QTest::newRow("sdk-delta")<<QStringLiteral("delta");
        QTest::newRow("sdk-bootstrap")<<QStringLiteral("bootstrap");
    }
    void exactRelationFeedsFacadeAndOldRevisionNeverFallsBack() {
        QFETCH(QString,relocation);
        Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("same-file.txt",{"A"},{"same","same"}));QVERIFY(f.bind());
        const auto record=f.session.sources().front();QVERIFY(setMdReference(record.modelDirectory));
        const auto entity=f.catalog.entities("well").front();const auto well=entity.extra.value("areaModelWellId").toLongLong();
        paleo::data::DataService service;std::string error;std::vector<paleo::data::DataEntry> entries;
        QVERIFY(service.openProject(record.modelDirectory.toStdString(),error));QVERIFY(service.listData("time_depth",well,entries,error));QCOMPARE(entries.size(),size_t(2));
        QVERIFY2(registerAreaSources(f.catalog,&f.error),qPrintable(f.error));
        paleo::data::DataObjectDescriptor descriptor;QVERIFY(service.describeDataObject(paleo::data::DataObjectKind::TimeDepth,entries[1].id,descriptor,error));
        RelationUseSpec use;use.context.documentId=f.session.documentId();use.context.modelStoreId="area-model";
        use.context.purpose=RelationPurpose::Correlation;use.wellEntityId=entity.id;use.modelWellId=well;
        ModelObjectRef object{record.ref,ObjectKind::TimeDepth,descriptor.id,QString::fromStdString(descriptor.revision)};
        SourceBinding binding;binding.id="second-relation";QVERIFY(setRelationBinding(binding,use,object,&f.error));
        auto command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=binding;QVERIFY2(invoke(f,command).ok,qPrintable(f.error));
        command=f.session.command(SourceCommand::Kind::ConfirmBinding);command.bindingId=binding.id;QVERIFY(invoke(f,command).ok);
        command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId=binding.id;QVERIFY(invoke(f,command).ok);
        QString requestError;const auto request=f.session.relationRequest(use,&requestError);QVERIFY2(requestError.isEmpty(),qPrintable(requestError));
        ProjectDataFacade facade;facade.setCatalog(&f.catalog,f.project);
        const auto rows=facade.tdTableFor(request);QVERIFY2(!rows.isEmpty(),qPrintable(facade.lastError()));
        QCOMPARE(rows.front().timeMs,220.);QCOMPARE(rows.front().md,100.);QCOMPARE(rows.front().timeDepthId,entries[1].id);
        QCOMPARE(rows.front().timeReference,QString("fixture"));
        QVERIFY(facade.tdTableFor(request,[]{return true;}).isEmpty());QVERIFY(!facade.lastError().isEmpty());
        paleo::area_model::registerProject(f.project,&f);
        const auto unregister=qScopeGuard([&] {paleo::area_model::unregisterProject(&f);});
        paleo::data::DataService pathReader;long long pathSource=0;
        QCOMPARE(paleo::area_model::openSource(record.path,pathReader,pathSource,&f.error),paleo::area_model::Read::Ready);
        QCOMPARE(pathSource,record.ref.sourceId);
        pathReader.closeProject();
        const auto moved=f.temp.filePath("moved.txt");QVERIFY(QFile::rename(record.path,moved));
        if(relocation=="session") {
            command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=moved;
            QVERIFY(invoke(f,command).ok);
        } else {
            std::shared_ptr<const paleo::data::PreparedSourceRelocation> prepared;
            QVERIFY2(service.prepareSourceRelocation(record.ref.sourceId,moved.toStdString(),prepared,error),error.c_str());
            QVERIFY2(service.commitSourceRelocation(*prepared,error),error.c_str());
            if(relocation=="bootstrap") {
                paleo::data::ModelChangeHead before,after;
                QVERIFY2(service.readModelChangeHead(before,error),error.c_str());
                QVERIFY2(service.forkModelGeneration(before,after,error),error.c_str());
            }
            AreaSourceProjectionStats stats;
            QVERIFY2(registerAreaSourcesWithStats(f.catalog,&f.error,&stats),qPrintable(f.error));
            if(relocation=="bootstrap") QCOMPARE(stats.bootstrapScans,1);
            else QVERIFY(stats.batches>0);
        }
        QCOMPARE(paleo::area_model::openSource(moved,pathReader,pathSource,&f.error),paleo::area_model::Read::Ready);
        QCOMPARE(pathSource,record.ref.sourceId);
        pathReader.closeProject();
        QVERIFY(facade.tdTableFor(request).isEmpty()); // Old alias metadata is no longer the current document choice.
        const auto movedRequest=f.session.relationRequest(use,&requestError);QVERIFY(requestError.isEmpty());
        const auto movedRows=facade.tdTableFor(movedRequest);QVERIFY2(!movedRows.isEmpty(),qPrintable(facade.lastError()));
        QCOMPARE(movedRows.front().timeMs,220.);
        paleo::data::DataObjectDescriptor first;QVERIFY(service.describeDataObject(paleo::data::DataObjectKind::TimeDepth,entries[0].id,first,error));
        SourceBinding replacement;replacement.id="first-relation";
        ModelObjectRef firstRef{record.ref,ObjectKind::TimeDepth,first.id,QString::fromStdString(first.revision)};
        QVERIFY(setRelationBinding(replacement,use,firstRef,&f.error));
        command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=replacement;QVERIFY(invoke(f,command).ok);
        command=f.session.command(SourceCommand::Kind::ConfirmBinding);command.bindingId=replacement.id;QVERIFY(invoke(f,command).ok);
        command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId=replacement.id;QVERIFY(invoke(f,command).ok);
        QVERIFY(facade.tdTableFor(movedRequest).isEmpty());
        const auto latest=f.session.relationRequest(use,&requestError);QVERIFY(requestError.isEmpty());
        const auto replacementRows=facade.tdTableFor(latest);QVERIFY2(!replacementRows.isEmpty(),qPrintable(facade.lastError()));
        QCOMPARE(replacementRows.front().timeMs,200.);
        QVERIFY(service.confirmTimeDepthSemantics(entries[1].id,"ms","TWT","changed-datum",false,error));
        QVERIFY(facade.tdTableFor(latest).isEmpty());QVERIFY(!facade.lastError().isEmpty());
        QCOMPARE(f.session.source(record.ref).fingerprint,record.fingerprint);
        auto other=use;other.context.purpose=RelationPurpose::Mapping;
        QVERIFY(!f.session.relationView(other).selected);
    }
};
QTEST_GUILESS_MAIN(TestDocumentRelationReader)
#include "tst_document_relation_reader.moc"
