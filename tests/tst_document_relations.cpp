#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "domain/documentrelation.h"
#include "services/documentrelationresolver.h"
#include "services/stratigraphicsubintervals.h"
#include <paleo_data/object_descriptor.h>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

using namespace document_fixture;
namespace {
QVariant sql(const QString &root,const QString &statement,const QVariantList &args={}) {
    const auto name="relation-test-"+QUuid::createUuid().toString();QVariant result;QString error;
    {
        auto db=QSqlDatabase::addDatabase("QSQLITE",name);db.setDatabaseName(root+"/data.sqlite");
        if(!db.open())error=db.lastError().text();else {
            QSqlQuery query(db);query.prepare(statement);for(const auto &arg:args)query.addBindValue(arg);
            if(!query.exec())error=query.lastError().text();else if(query.next())result=query.value(0);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);if(!error.isEmpty())qFatal("%s",qPrintable(error));return result;
}
struct RelationFixture {
    Fixture base;
    RelationResolveRequest request;
    qlonglong otherWellObject=0,trackId=0;
    bool init() {
        if(!base.init() || !base.preparedWellSource("multi.txt",{"A","B"},{"same","same"},{"same","same"}) || !base.bind())return false;
        const auto source=base.session.sources().front();const auto well=base.catalog.wellsMatchingName("A").front();
        const auto modelWell=base.catalog.entityById(well).extra.value("areaModelWellId").toLongLong();
        paleo::data::DataService reader;std::string error;std::vector<paleo::data::DataEntry> entries;
        if(!reader.openProjectReadOnly(source.modelDirectory.toStdString(),error) || !reader.listData("time_depth",modelWell,entries,error) || entries.size()!=2)return false;
        paleo::data::DataObjectDescriptor object;
        if(!reader.describeDataObject(paleo::data::DataObjectKind::TimeDepth,entries[1].id,object,error))return false;
        InterpretationContext context;context.documentId=source.ref.documentId;context.modelStoreId=source.ref.modelStoreId;context.purpose=RelationPurpose::WellTie;
        request.use={context,well,modelWell,RelationKind::TimeDepth};request.object={source.ref,ObjectKind::TimeDepth,object.id,QString::fromStdString(object.revision)};
        request.selected=source;request.catalogDocumentId=context.documentId;request.wellEntityId=well;request.mappedModelWellId=modelWell;
        if(!reader.listSourceData("time_depth",source.ref.sourceId,entries,error))return false;
        for(const auto &entry:entries)if(entry.wellId!=modelWell)otherWellObject=entry.id;
        if(!reader.listData("intervals",modelWell,entries,error) || entries.empty())return false;
        trackId=entries.front().id;return otherWellObject>0 && object.revisionKnown;
    }
};
}
class TestDocumentRelations:public QObject {
    Q_OBJECT
private slots:
    void opaqueRecordGroupsDoNotBecomeTypedThroughFieldCollisions() {
        SourceBinding legacy;legacy.id="legacy";legacy.resourceKind="well";legacy.resourceId="well-1";legacy.role="time_depth";
        legacy.source={"document","area-model",1,QString(64,'a')};
        legacy.locator={LocatorKind::RecordGroup,{{"key","time_depth/1"},{"schema",17},{"use","old display purpose"},{"objectRef","old record label"}}};
        for(const auto &scope:QStringList{"opaque-scope","interpretation:legacy","interpretation:"+QString(64,'0')}) {
            legacy.scope=scope;QString error;QVERIFY(legacy.valid(&error));QVERIFY(!hasRelationBinding(legacy));
            SourceBinding restored;QVERIFY(sourceBindingFromJson(sourceBindingJson(legacy),restored,&error));
            QVERIFY(!hasRelationBinding(restored));QCOMPARE(restored,legacy);
            RelationUseSpec use;ModelObjectRef object;QVERIFY(!relationBinding(restored,use,object,&error));
        }
    }
    void futureRelationMarkersAndSchemasCannotFallBackToLegacy() {
        RelationUseSpec use;use.context.documentId="document";use.context.modelStoreId="area-model";
        use.wellEntityId="well-1";use.modelWellId=1;
        ModelObjectRef object{{"document","area-model",1,QString(64,'a')},ObjectKind::TimeDepth,1,"epoch:1"};
        SourceBinding binding;binding.id="typed";QString error;QVERIFY(setRelationBinding(binding,use,object,&error));
        for(int variant=0;variant<3;++variant) {
            auto future=binding;
            if(variant!=0)future.locator.fields.insert("key","document-relation-v2");
            if(variant!=2)future.locator.fields.insert("schema",2);
            QVERIFY(hasRelationBinding(future));RelationUseSpec decodedUse;ModelObjectRef decodedObject;
            QVERIFY(!relationBinding(future,decodedUse,decodedObject,&error));QVERIFY(!error.isEmpty());
            QVERIFY(!decodedUse.valid());QVERIFY(!decodedObject.valid());
        }
    }
    void typedIdentityRoundTripsAndReadsExactSecondRelation() {
        RelationFixture f;QVERIFY(f.init());auto &request=f.request;
        const auto resolved=resolveDocumentRelation(request);QVERIFY2(resolved.ok,qPrintable(resolved.error));
        SourceBinding binding;binding.id="typed";QString error;
        QVERIFY(setRelationBinding(binding,request.use,request.object,&error));QVERIFY(binding.valid(&error));QVERIFY(hasRelationBinding(binding));
        SourceBinding restored;QVERIFY(sourceBindingFromJson(sourceBindingJson(binding),restored,&error));
        RelationUseSpec use;ModelObjectRef object;QVERIFY(relationBinding(restored,use,object,&error));QCOMPARE(use,request.use);QCOMPARE(object,request.object);
        for(const auto &schema:QVariantList{QString("1"),true,1.0}) {
            auto malformed=restored;malformed.locator.fields.insert("schema",schema);
            QVERIFY(!relationBinding(malformed,use,object,&error));
        }
        restored.locator.fields.insert("schema",QVariant::fromValue(qlonglong(1)));
        QVERIFY(relationBinding(restored,use,object,&error));
        auto other=use.context;other.purpose=RelationPurpose::Mapping;QVERIFY(other.key()!=use.context.key());
        paleo::data::DataService reader;std::string detail;QVERIFY(reader.openProjectReadOnly(request.selected.modelDirectory.toStdString(),detail));
        paleo::data::TimeDepthTable table;QVERIFY2(reader.readTimeDepthAtRevision(object.id,object.revision.toStdString(),table,detail),detail.c_str());
        QCOMPARE(table.depthTimePairs.front().timeValue,220.);
        auto legacy=binding;legacy.scope="scheme-A@r1/well-tie/volume-1@r1";legacy.locator.fields={{"key","time_depth/1"}};
        QVERIFY(!hasRelationBinding(legacy));QVERIFY(!relationBinding(legacy,use,object,&error));
        restored.locator.fields.remove("objectRef");QVERIFY(hasRelationBinding(restored));QVERIFY(!relationBinding(restored,use,object,&error));
    }
    void invalidOwnershipAndRevisionsAreRejected_data() {
        QTest::addColumn<QString>("fault");QTest::addColumn<QString>("code");
        QTest::newRow("other-well-object")<<QString("well")<<QString("object_owner_mismatch");
        QTest::newRow("unmapped-well")<<QString("mapping")<<QString("model_well_mapping_missing");
        QTest::newRow("native-no-map")<<QString("native")<<QString("model_well_mapping_missing");
        QTest::newRow("source")<<QString("source")<<QString("source_revision_changed");
        QTest::newRow("object")<<QString("object")<<QString("object_missing");
        QTest::newRow("revision")<<QString("revision")<<QString("object_revision_changed");
        QTest::newRow("document")<<QString("document")<<QString("relation_invalid");
        QTest::newRow("inactive")<<QString("inactive")<<QString("source_revision_changed");
        QTest::newRow("source-sha")<<QString("sha")<<QString("source_revision_changed");
    }
    void invalidOwnershipAndRevisionsAreRejected() {
        QFETCH(QString,fault);QFETCH(QString,code);RelationFixture f;QVERIFY(f.init());auto &r=f.request;
        if(fault=="well")r.object.id=f.otherWellObject;
        if(fault=="mapping")++r.mappedModelWellId;
        if(fault=="native") {r.object.source.modelStoreId="native/data.sqlite";r.selected.ref=r.object.source;r.use.context.modelStoreId=r.object.source.modelStoreId;}
        if(fault=="source")++r.object.source.sourceId;
        if(fault=="object")r.object.id=999999;
        if(fault=="revision")r.object.revision="different";
        if(fault=="document")r.catalogDocumentId="other";
        if(fault=="inactive")sql(r.selected.modelDirectory,"UPDATE sources SET active=0 WHERE id=?",{r.selected.ref.sourceId});
        if(fault=="sha")sql(r.selected.modelDirectory,"UPDATE sources SET hash=? WHERE id=?",{QString(64,'f'),r.selected.ref.sourceId});
        const auto result=resolveDocumentRelation(r);QVERIFY(!result.ok);QCOMPARE(result.code,code);
    }
    void fixedSchemeAndTargetVolumeAreCheckedWithoutSourceReads() {
        RelationFixture f;QVERIFY(f.init());auto &r=f.request;
        paleo::data::DataService writer;std::string detail;QVERIFY(writer.openProject(r.selected.modelDirectory.toStdString(),detail));
        auto draft=paleo::strat::draftScheme("scheme","synthetic",{"T1","T2","T3"},{});
        paleo::data::StratigraphicScheme scheme;std::vector<paleo::data::FrameworkIssue> issues;
        QVERIFY2(writer.commitStratigraphicScheme(draft,{},scheme,issues,detail),detail.c_str());
        const auto choices=paleo::strat::selectorOptions(scheme);QCOMPARE(choices.size(),2);
        r.use.context.schemeId=QString::fromStdString(scheme.id);r.use.context.schemeRevision=QString::fromStdString(scheme.revision.value);
        r.use.context.intervalId=choices[0].intervalId;r.use.context.subIntervalId=choices[0].subIntervalIds[0];
        sql(r.selected.modelDirectory,"INSERT INTO volumes(source_id,name,path,geometry,sampling) VALUES(?,'volume','offline.sgy','invalid geometry','invalid sampling')",{r.selected.ref.sourceId});
        const auto volumeId=sql(r.selected.modelDirectory,"SELECT max(id) FROM volumes").toLongLong();paleo::data::DataObjectDescriptor volume;
        QVERIFY(writer.describeDataObject(paleo::data::DataObjectKind::SeismicVolume,volumeId,volume,detail));
        r.use.context.targetVolume=ModelObjectRef{r.selected.ref,ObjectKind::SeismicVolume,volumeId,QString::fromStdString(volume.revision)};r.targetVolume=r.selected;
        QVERIFY(QFile::remove(r.selected.path));auto result=resolveDocumentRelation(r);QVERIFY2(result.ok,qPrintable(result.error));
        const auto correct=r.use.context;
        r.use.context.subIntervalId=choices[1].subIntervalIds[0];QCOMPARE(resolveDocumentRelation(r).code,QString("context_revision_invalid"));
        r.use.context=correct;r.use.context.schemeRevision="missing";QCOMPARE(resolveDocumentRelation(r).code,QString("context_revision_invalid"));
        r.use.context=correct;sql(r.selected.modelDirectory,"UPDATE volumes SET sampling='changed' WHERE id=?",{volumeId});
        QCOMPARE(resolveDocumentRelation(r).code,QString("object_revision_changed"));
    }
    void intervalTrackResolvesAndSemanticChangesInvalidateOldReference() {
        RelationFixture f;QVERIFY(f.init());auto &r=f.request;
        paleo::data::DataService writer;std::string detail;QVERIFY(writer.openProject(r.selected.modelDirectory.toStdString(),detail));
        paleo::data::DataObjectDescriptor track;QVERIFY(writer.describeDataObject(paleo::data::DataObjectKind::IntervalTrack,f.trackId,track,detail));
        r.object={r.selected.ref,ObjectKind::IntervalTrack,f.trackId,QString::fromStdString(track.revision)};r.use.relation=RelationKind::StratigraphicTrack;
        QVERIFY(resolveDocumentRelation(r).ok);sql(r.selected.modelDirectory,"UPDATE intervals SET bottom=205 WHERE track_id=?",{f.trackId});
        QCOMPARE(resolveDocumentRelation(r).code,QString("object_revision_changed"));
    }
    void volumeRelocationPreservesTypedContextAndSourceIdentity() {
        RelationFixture f;QVERIFY(f.init());auto &r=f.request;const auto original=r.selected.ref;
        sql(r.selected.modelDirectory,"INSERT INTO volumes(source_id,name,path,geometry,sampling) VALUES(?,'volume',?,'{}','{}')",{original.sourceId,r.selected.path});
        const auto id=sql(r.selected.modelDirectory,"SELECT max(id) FROM volumes").toLongLong();
        paleo::data::DataService reader;std::string detail;QVERIFY(reader.openProjectReadOnly(r.selected.modelDirectory.toStdString(),detail));
        paleo::data::DataObjectDescriptor before;QVERIFY(reader.describeDataObject(paleo::data::DataObjectKind::SeismicVolume,id,before,detail));
        r.use.context.targetVolume=ModelObjectRef{original,ObjectKind::SeismicVolume,id,QString::fromStdString(before.revision)};
        r.targetVolume=r.selected;QVERIFY(resolveDocumentRelation(r).ok);const auto contextKey=r.use.context.key();
        const auto candidate=f.base.temp.filePath("moved.txt");QVERIFY(QFile::rename(r.selected.path,candidate));
        auto command=f.base.session.command(SourceCommand::Kind::Relocate);command.source=original;command.path=candidate;
        QSignalSpy finished(&f.base.session,&DocumentSession::commandFinished);QVERIFY(f.base.session.execute(command).pending);QTRY_COMPARE(finished.size(),1);
        const auto relocated=qvariant_cast<SourceCommandResult>(finished.front().front());QVERIFY2(relocated.ok,qPrintable(relocated.error));
        r.selected=f.base.session.source(original);r.targetVolume=r.selected;QCOMPARE(r.selected.ref,original);
        paleo::data::DataObjectDescriptor after;QVERIFY(reader.describeDataObject(paleo::data::DataObjectKind::SeismicVolume,id,after,detail));
        QCOMPARE(after.revision,before.revision);QCOMPARE(r.use.context.key(),contextKey);
        const auto result=resolveDocumentRelation(r);QVERIFY2(result.ok,qPrintable(result.error));
    }
    void legacyReadonlyModelRequiresExplicitUpgrade() {
        RelationFixture f;QVERIFY(f.init());const auto &root=f.request.selected.modelDirectory;
        for(const auto &table:QStringList{"time_depth","tracks","intervals","volumes"})
            for(const auto &operation:QStringList{"ai","au","ad"})sql(root,"DROP TRIGGER pds_revision_"+table+"_"+operation);
        sql(root,"DROP TABLE object_revisions");sql(root,"DELETE FROM ds_meta WHERE key IN ('object_revision_schema','object_revision_epoch')");
        QCOMPARE(resolveDocumentRelation(f.request).code,QString("object_revision_upgrade_required"));
        QCOMPARE(sql(root,"SELECT count(*) FROM sqlite_master WHERE name='object_revisions'").toInt(),0);
    }
};
QTEST_GUILESS_MAIN(TestDocumentRelations)
#include "tst_document_relations.moc"
