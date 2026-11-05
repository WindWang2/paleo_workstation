#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "domain/documentrelation.h"
#include <paleo_data/object_descriptor.h>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

using namespace document_fixture;
namespace {
bool awaitResult(DocumentSession &session,const std::function<SourceCommandResult()> &start,SourceCommandResult &done,QString &error) {
    QSignalSpy spy(&session,&DocumentSession::commandFinished);const auto pending=start();
    if(!pending.pending||pending.ok){error="expected asynchronous pending result: "+pending.code+" / "+pending.error;return false;}
    QElapsedTimer timer;timer.start();int count=0;
    while(timer.elapsed()<10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents,10);count=0;
        for(const auto &row:spy) {const auto value=qvariant_cast<SourceCommandResult>(row[0]);if(value.operationId==pending.operationId){done=value;++count;}}
        if(count)break;QTest::qWait(2);
    }
    QTest::qWait(5);count=0;
    for(const auto &row:spy)if(qvariant_cast<SourceCommandResult>(row[0]).operationId==pending.operationId)++count;
    if(count!=1){error="relation operation must finish exactly once; count="+QString::number(count);return false;}
    return true;
}
bool executeAsync(DocumentSession &session,const SourceCommand &command,SourceCommandResult &done,QString &error) {
    return awaitResult(session,[&]{return session.execute(command);},done,error);
}
QByteArray digest(const QString &path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256);}
struct RelationFixture:Fixture {
    RelationUseSpec use;
    SourceRecord selected;
    QVector<ModelObjectRef> objects;
    paleo::data::StratigraphicScheme scheme;
    bool setup() {
        if(!init()||!preparedWellSource("same-named-relations.txt",{"A"},{"same relation","same relation"},{"same track","same track"}))return false;
        paleo::data::DataService model;std::string why;
        if(!model.openProject(paleo::area_model::directory(project).toStdString(),why)){error=QString::fromStdString(why);return false;}
        scheme.id="session-scheme";scheme.name="synthetic session context";
        for(int i=0;i<2;++i){paleo::data::StratigraphicBoundary boundary;boundary.name=i?"BASE":"TOP";boundary.order=i+1;scheme.boundaries.push_back(boundary);}
        std::vector<paleo::data::FrameworkIssue> issues;paleo::data::StratigraphicScheme committed;
        if(!model.commitStratigraphicScheme(scheme,{},committed,issues,why)){error=QString::fromStdString(why);return false;}scheme=committed;
        const auto well=catalog.wellsMatchingName("A").value(0);const auto modelWell=catalog.entityById(well).extra.value("areaModelWellId").toLongLong();
        std::vector<paleo::data::DataEntry> relations;
        if(!model.listData("time_depth",modelWell,relations,why)||relations.size()!=2){error="two relation objects required";return false;}
        if(!bind())return false;
        for(const auto &record:session.sources())if(record.ref.modelStoreId=="area-model"&&record.ref.sourceId==relations[0].sourceId)selected=record;
        if(!selected.ref.valid()){error="source projection missing";return false;}
        use.context.documentId=session.documentId();use.context.modelStoreId="area-model";use.context.purpose=RelationPurpose::Correlation;
        use.context.schemeId=QString::fromStdString(scheme.id);use.context.schemeRevision=QString::fromStdString(scheme.revision.value);
        use.context.intervalId=QString::fromStdString(scheme.intervals[0].id.value);use.context.subIntervalId=QString::fromStdString(scheme.subIntervals[0].id.value);
        use.wellEntityId=well;use.modelWellId=modelWell;
        for(const auto &relation:relations) {
            paleo::data::DataObjectDescriptor descriptor;
            if(!model.describeDataObject(paleo::data::DataObjectKind::TimeDepth,relation.id,descriptor,why)||!descriptor.revisionKnown){error=QString::fromStdString(why);return false;}
            objects.append({selected.ref,ObjectKind::TimeDepth,relation.id,QString::fromStdString(descriptor.revision)});
        }
        return use.valid();
    }
    SourceBinding binding(int index=1,const QString &id="typed-choice") {
        SourceBinding value;value.id=id;setRelationBinding(value,use,objects[index],&error);return value;
    }
    bool choose(int index=1,const QString &id="typed-choice") {
        SourceCommandResult done;auto command=session.command(SourceCommand::Kind::RegisterBinding);command.binding=binding(index,id);
        if(!executeAsync(session,command,done,error)||!done.ok){if(error.isEmpty())error=done.error;return false;}
        command=session.command(SourceCommand::Kind::ConfirmBinding);command.bindingId=id;command.note="explicit relation identity choice";
        if(!executeAsync(session,command,done,error)||!done.ok){if(error.isEmpty())error=done.error;return false;}
        command=session.command(SourceCommand::Kind::SelectPrimary);command.bindingId=id;
        if(!executeAsync(session,command,done,error)||!done.ok){if(error.isEmpty())error=done.error;return false;}return true;
    }
};
}

class TestDocumentRelationSession:public QObject {
    Q_OBJECT
private slots:
    void typedChoicesAndEveryResolutionAreAsynchronous() {
        RelationFixture f;QVERIFY2(f.setup(),qPrintable(f.error));const auto before=digest(f.selected.path);
        QVERIFY2(f.choose(),qPrintable(f.error));const auto view=f.session.relationView(f.use);
        QVERIFY(view.selected);QCOMPARE(view.selectedObject,f.objects[1]);QCOMPARE(view.code,QString("selection_unvalidated"));
        QVERIFY(view.detail.contains("不代表"));QString error;const auto request=f.session.relationRequest(f.use,&error);
        QVERIFY2(error.isEmpty(),qPrintable(error));QCOMPARE(request.object,f.objects[1]);QCOMPARE(request.mappedModelWellId,f.use.modelWellId);
        for(int i=0;i<2;++i) {
            SourceCommandResult done;QVERIFY2(awaitResult(f.session,[&]{return f.session.resolveRelation(f.use);},done,error),qPrintable(error));
            QVERIFY2(done.ok,qPrintable(done.error));QCOMPARE(done.code,QString("relation_resolved"));
            ModelObjectRef object;QVERIFY(modelObjectRefFromJson(QJsonObject::fromVariantMap(done.details.value("objectRef").toMap()),object,&error));QCOMPARE(object,f.objects[1]);
            RelationUseSpec use;QVERIFY(relationUseSpecFromJson(QJsonObject::fromVariantMap(done.details.value("use").toMap()),use,&error));QCOMPARE(use,f.use);
        }
        QCOMPARE(digest(f.selected.path),before);
        paleo::data::DataService reader;std::string why;paleo::data::TimeDepthTable table;
        QVERIFY(reader.openProjectReadOnly(f.selected.modelDirectory.toStdString(),why));QVERIFY(reader.readTimeDepth(f.objects[1].id,table,why));
        QVERIFY(table.depthInfo.mdReference.empty()); // Relation choice never confirms missing physical declarations.
    }
    void semanticEditsInvalidateObjectsEvenWithUnchangedRawFingerprint() {
        RelationFixture f;QVERIFY2(f.setup(),qPrintable(f.error));QVERIFY2(f.choose(),qPrintable(f.error));const auto before=digest(f.selected.path);
        paleo::data::DataService editor;std::string why;QVERIFY(editor.openProject(f.selected.modelDirectory.toStdString(),why));
        QVERIFY(editor.confirmTimeDepthSemantics(f.objects[1].id,"ms","TWT","new-confirmed-datum",false,why));editor.closeProject();
        QCOMPARE(digest(f.selected.path),before);QVERIFY(f.session.relationView(f.use).selected);
        SourceCommandResult done;QString error;QVERIFY2(awaitResult(f.session,[&]{return f.session.resolveRelation(f.use);},done,error),qPrintable(error));
        QVERIFY(!done.ok);QCOMPARE(done.code,QString("object_revision_changed"));
    }
    void typedStratigraphicTrackUsesItsActualObjectIdentity() {
        RelationFixture f;QVERIFY2(f.setup(),qPrintable(f.error));paleo::data::DataService reader;std::string why;
        QVERIFY(reader.openProjectReadOnly(f.selected.modelDirectory.toStdString(),why));std::vector<paleo::data::DataEntry> tracks;
        QVERIFY(reader.listData("intervals",f.use.modelWellId,tracks,why));QCOMPARE(tracks.size(),size_t(2));QCOMPARE(tracks[0].name,tracks[1].name);
        paleo::data::DataObjectDescriptor descriptor;QVERIFY(reader.describeDataObject(paleo::data::DataObjectKind::IntervalTrack,tracks[1].id,descriptor,why));reader.closeProject();
        f.use.relation=RelationKind::StratigraphicTrack;f.objects[1]={f.selected.ref,ObjectKind::IntervalTrack,tracks[1].id,QString::fromStdString(descriptor.revision)};
        QVERIFY2(f.choose(),qPrintable(f.error));QCOMPARE(f.session.relationView(f.use).selectedObject.id,qlonglong(tracks[1].id));
        SourceCommandResult done;QString error;QVERIFY2(awaitResult(f.session,[&]{return f.session.resolveRelation(f.use);},done,error),qPrintable(error));QVERIFY2(done.ok,qPrintable(done.error));
    }
    void newSchemeRevisionAndOpaqueLegacyScopesNeverInheritTypedChoice() {
        RelationFixture f;QVERIFY2(f.setup(),qPrintable(f.error));
        auto legacy=f.binding();legacy.id="opaque";legacy.locator={LocatorKind::RecordGroup,{{"key","time_depth/old-name"}}};
        auto command=f.session.command(SourceCommand::Kind::RegisterBinding);auto future=legacy;future.id="future-marker";
        future.locator.fields["key"]="document-relation-v2";command.binding=future;
        auto result=f.session.execute(command);QVERIFY(!result.ok&&!result.pending);QCOMPARE(result.code,QString("relation_binding_invalid"));
        // Opaque old locators stay legacy even when their scope resembles a typed key.
        command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=legacy;result=f.session.execute(command);
        QVERIFY2(result.ok,qPrintable(result.code+" / "+result.error));QVERIFY(!result.pending);
        for(const auto kind:{SourceCommand::Kind::ConfirmBinding,SourceCommand::Kind::SelectPrimary}){
            command=f.session.command(kind);command.bindingId=legacy.id;result=f.session.execute(command);QVERIFY2(result.ok,qPrintable(result.error));
        }
        QVERIFY(!f.session.relationView(f.use).selected);QVERIFY(f.session.relationView(f.use).choices.empty());
        QVERIFY2(f.choose(),qPrintable(f.error));
        paleo::data::DataService editor;std::string why;QVERIFY(editor.openProject(f.selected.modelDirectory.toStdString(),why));
        auto changed=f.scheme;changed.note="new interpretation context";paleo::data::StratigraphicScheme next;std::vector<paleo::data::FrameworkIssue> issues;
        QVERIFY(editor.commitStratigraphicScheme(changed,f.scheme.revision.value,next,issues,why));editor.closeProject();
        auto use=f.use;use.context.schemeRevision=QString::fromStdString(next.revision.value);
        QVERIFY(!f.session.relationView(use).selected);QVERIFY(f.session.relationView(use).choices.empty());
        result=f.session.resolveRelation(use);QVERIFY(!result.ok&&!result.pending);QCOMPARE(result.code,QString("relation_selection_missing"));
    }
    void readonlyAllowsResolutionButRefusesMetadataUpgrade() {
        RelationFixture f;QVERIFY2(f.setup(),qPrintable(f.error));QVERIFY2(f.choose(),qPrintable(f.error));f.session.close();QVERIFY(f.bind(true));
        SourceCommandResult done;QString error;QVERIFY2(awaitResult(f.session,[&]{return f.session.resolveRelation(f.use);},done,error),qPrintable(error));QVERIFY2(done.ok,qPrintable(done.error));
        auto command=f.session.command(SourceCommand::Kind::UpgradeModelMetadata);command.source=f.selected.ref;const auto result=f.session.execute(command);
        QVERIFY(!result.ok&&!result.pending);QCOMPARE(result.code,QString("read_only"));
    }
    void explicitUpgradeIsAsyncAndReadonlyLookupDoesNotMigrate() {
        RelationFixture f;QVERIFY2(f.setup(),qPrintable(f.error));QVERIFY2(f.choose(),qPrintable(f.error));
        const auto connection="legacy_revision_fixture_"+QUuid::createUuid().toString(QUuid::WithoutBraces);
        {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(f.selected.modelDirectory+"/data.sqlite");QVERIFY(db.open());
         {QSqlQuery query(db);QVERIFY(query.exec("DELETE FROM ds_meta WHERE key IN ('object_revision_schema','object_revision_epoch')"));}db.close();}
        QSqlDatabase::removeDatabase(connection);const auto before=digest(f.selected.modelDirectory+"/data.sqlite");
        SourceCommandResult done;QString error;QVERIFY2(awaitResult(f.session,[&]{return f.session.resolveRelation(f.use);},done,error),qPrintable(error));
        QVERIFY(!done.ok);QCOMPARE(done.code,QString("object_revision_upgrade_required"));QCOMPARE(digest(f.selected.modelDirectory+"/data.sqlite"),before);
        auto command=f.session.command(SourceCommand::Kind::UpgradeModelMetadata);command.source=f.selected.ref;
        QVERIFY2(executeAsync(f.session,command,done,error),qPrintable(error));QVERIFY2(done.ok,qPrintable(done.error));
        QCOMPARE(done.code,QString("model_metadata_upgraded"));QVERIFY(done.details.value("upgraded").toBool());
        paleo::data::DataService reader;std::string why;paleo::data::DataObjectDescriptor descriptor;
        QVERIFY(reader.openProjectReadOnly(f.selected.modelDirectory.toStdString(),why));QVERIFY(reader.describeDataObject(paleo::data::DataObjectKind::TimeDepth,f.objects[1].id,descriptor,why));QVERIFY(descriptor.revisionKnown);
    }
    void invalidIDsAndMissingWellMappingNeverUseDisplayNames() {
        RelationFixture f;QVERIFY2(f.setup(),qPrintable(f.error));auto binding=f.binding();
        auto object=f.objects[1];object.id+=1000000;QVERIFY(setRelationBinding(binding,f.use,object,&f.error));
        auto command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=binding;SourceCommandResult done;QString error;
        QVERIFY2(executeAsync(f.session,command,done,error),qPrintable(error));QVERIFY(!done.ok);QCOMPARE(done.code,QString("object_missing"));
        QVERIFY(f.session.relationView(f.use).choices.empty());
        binding=f.binding();binding.locator.fields["schema"]=QString("1");command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=binding;
        const auto malformed=f.session.execute(command);QVERIFY(!malformed.ok&&!malformed.pending);QCOMPARE(malformed.code,QString("relation_binding_invalid"));
        auto well=f.catalog.storedEntityById(f.use.wellEntityId);well.extra.remove("areaModelWellId");QVERIFY2(f.catalog.updateEntity(well,&error),qPrintable(error));
        QVERIFY(!f.catalog.storedEntityById(f.use.wellEntityId).extra.contains("areaModelWellId"));
        QVERIFY(f.catalog.entityById(f.use.wellEntityId).extra.value("areaModelWellId").toLongLong()>0); // Legacy projection would guess by name.
        command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=f.binding();const auto unmapped=f.session.execute(command);
        QVERIFY(!unmapped.ok&&!unmapped.pending);QCOMPARE(unmapped.code,QString("relation_request_invalid"));
    }
    void closeCancelAndCatalogChangeDiscardInFlightWritesExactlyOnce() {
        RelationFixture f;QVERIFY2(f.setup(),qPrintable(f.error));QSignalSpy finished(&f.session,&DocumentSession::commandFinished);
        auto command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=f.binding();auto pending=f.session.execute(command);QVERIFY(pending.pending);
        f.session.close();QCOMPARE(finished.size(),1);QCOMPARE(qvariant_cast<SourceCommandResult>(finished.front()[0]).operationId,pending.operationId);
        QTRY_COMPARE(f.tasks.runningCount(),0);QTest::qWait(20);QCOMPARE(finished.size(),1);QVERIFY(f.bind());QVERIFY(f.session.relationView(f.use).choices.empty());
        command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=f.binding();pending=f.session.execute(command);QVERIFY(pending.pending);
        for(auto *task:f.tasks.tasks())if(task&&task->state()!=PaleoTask::State::Succeeded&&task->state()!=PaleoTask::State::Failed&&task->state()!=PaleoTask::State::Cancelled)task->requestCancel();
        QTRY_COMPARE(finished.size(),2);QVERIFY(!qvariant_cast<SourceCommandResult>(finished.back()[0]).ok);QVERIFY(f.session.relationView(f.use).choices.empty());
        command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=f.binding();SourceCommandResult done;QString error,mutationError;bool changed=false;
        QVERIFY2(awaitResult(f.session,[&]{
            auto result=f.session.execute(command);changed=f.catalog.updateVersionExtra(f.selected.versionId,"testRevision",1,&mutationError);return result;
        },done,error),qPrintable(error));
        QVERIFY2(changed,qPrintable(mutationError));
        QVERIFY(!done.ok);QCOMPARE(done.code,QString("stale_command"));QVERIFY(f.session.relationView(f.use).choices.empty());
    }
};
QTEST_GUILESS_MAIN(TestDocumentRelationSession)
#include "tst_document_relation_session.moc"
