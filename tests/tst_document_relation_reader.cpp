#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "domain/documentrelation.h"
#include "services/projectdata.h"
#include <paleo_data/object_descriptor.h>
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
    void exactRelationFeedsFacadeAndOldRevisionNeverFallsBack() {
        Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("same-file.txt",{"A"},{"same","same"}));QVERIFY(f.bind());
        const auto record=f.session.sources().front();QVERIFY(setMdReference(record.modelDirectory));
        const auto entity=f.catalog.entities("well").front();const auto well=entity.extra.value("areaModelWellId").toLongLong();
        paleo::data::DataService service;std::string error;std::vector<paleo::data::DataEntry> entries;
        QVERIFY(service.openProject(record.modelDirectory.toStdString(),error));QVERIFY(service.listData("time_depth",well,entries,error));QCOMPARE(entries.size(),size_t(2));
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
        const auto moved=f.temp.filePath("moved.txt");QVERIFY(QFile::rename(record.path,moved));
        command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=moved;
        QVERIFY(invoke(f,command).ok);
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
