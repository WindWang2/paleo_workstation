#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "services/documentsourceprobe.h"
#include "io/dataimportservice.h"
#include "io/welllogread.h"
#include "metadata/documentsourcestore.h"
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>

using namespace document_fixture;
class TestDocumentRelocation:public QObject {
    Q_OBJECT
    struct NativeFixture {
        QTemporaryDir temp;
        QString project=temp.filePath("project"),meta=project+"/metadata/project.sqlite",path=temp.filePath("source.las"),error;
        PaleoProjectStore store;PaleoTaskService tasks{&store};DataImportService imports{&store};DocumentSession session{&store,&tasks};
        CatalogVersion original;
        bool init() {
            if(!write(path,"~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. SYNTH-1\nNULL. -999.25\n~Curve\nDEPT.M : depth\nGR.API : gamma\n~Ascii\n100 -999\n101 -999.25\n102 -20\n"))return false;
            store.setProjectPaths(project+"/project.qgz",project+"/project.gpkg",meta);imports.setProjectDir(project);
            DataImportService::ImportOptions options;options.linkExternal=true;options.forceType="well_log";options.targetWellId="new:SYNTH-1";
            if(imports.importProjectFileEx(path,options,&error).outcome!=DataImportService::ImportOutcome::Imported)return false;
            original=imports.catalog()->versions().front();
            return !original.extra.value("nativeModel").toMap().isEmpty() && session.bind(imports.catalog(),meta,"native-regression",false,&error);
        }
        QByteArray databaseHash(const QVariantMap &binding) {
            QFile file(QDir(project).filePath(binding.value("path").toString()));
            return file.open(QIODevice::ReadOnly)?QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256):QByteArray();
        }
    };
    static QString modelRoot(const Fixture &f) {return paleo::area_model::directory(f.project);}
    static SourceRecord seismic(Fixture &f) {
        const auto path=f.temp.filePath("original.sgy");
        if(!write(path,smallSegy()) || !f.importModel(path) || !f.bind()) return {};
        const auto sources=f.session.sources();return sources.isEmpty()?SourceRecord{}:sources.front();
    }
private slots:
    void quickRelocationUpdatesModelAndCatalogWithoutFullHash() {
        Fixture f;QVERIFY(f.init());const auto original=seismic(f);QVERIFY2(original.ref.valid(),qPrintable(f.error));
        QCOMPARE(original.fingerprint.kind,FingerprintKind::SegyQuickV1);
        const auto candidate=f.temp.filePath("elsewhere/renamed.bin");QVERIFY(QDir().mkpath(QFileInfo(candidate).absolutePath()));
        QVERIFY(QFile::rename(original.path,candidate));
        QSignalSpy finished(&f.session,&DocumentSession::commandFinished);
        auto command=f.session.command(SourceCommand::Kind::Relocate);command.source=original.ref;command.path=candidate;
        QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(finished.size(),1);
        const auto result=qvariant_cast<SourceCommandResult>(finished.front().front());QVERIFY2(result.ok,qPrintable(result.error));
        QVERIFY(result.state.bytesRead<=65536);QVERIFY(!result.versionId.isEmpty());QVERIFY(result.versionId!=original.versionId);
        QCOMPARE(f.catalog.versionById(original.versionId).path,original.path);QCOMPARE(f.catalog.currentVersion(original.assetId).path,candidate);
        QCOMPARE(f.session.source(original.ref).path,candidate);QVERIFY(!f.session.recoveryRequired());
        paleo::data::DataService service;std::string error;QVERIFY(service.openProjectReadOnly(modelRoot(f).toStdString(),error));
        std::string path;QVERIFY(service.sourcePath(original.ref.sourceId,path,error));QCOMPARE(QString::fromUtf8(path),candidate);
        QVERIFY2(service.verifySource(original.ref.sourceId,error),error.c_str());
        std::vector<paleo::data::DataEntry> volumes;QVERIFY(service.listData("volumes",0,volumes,error));
        paleo::data::SeismicVolume volume;QVERIFY(service.readVolume(volumes.front().id,volume,error));QCOMPARE(QString::fromUtf8(volume.dataFile),candidate);
    }
    void changedCandidateAndCrossProjectPlanAreRejected() {
        Fixture f;QVERIFY(f.init());const auto record=seismic(f);QVERIFY(record.ref.valid());
        const auto candidate=f.temp.filePath("candidate.sgy");QVERIFY(QFile::copy(record.path,candidate));
        QFile file(candidate);QVERIFY(file.open(QIODevice::ReadWrite));QVERIFY(file.setFileTime(QFileInfo(record.path).lastModified(),QFileDevice::FileModificationTime));file.close();
        paleo::data::DataService reader;std::string error;QVERIFY(reader.openProjectReadOnly(modelRoot(f).toStdString(),error));
        std::shared_ptr<const paleo::data::PreparedSourceRelocation> prepared;
        QVERIFY2(reader.prepareSourceRelocation(record.ref.sourceId,candidate.toStdString(),prepared,error),error.c_str());
        QVERIFY(prepared->verifiedBytes()<=65536);QVERIFY(!reader.commitSourceRelocation(*prepared,error));
        paleo::data::DataService other;QVERIFY(other.createProject(f.temp.filePath("other-model").toStdString(),"other",error));
        QVERIFY(!other.commitSourceRelocation(*prepared,error));
        QVERIFY(file.open(QIODevice::Append));QCOMPARE(file.write("changed"),qint64(7));file.close();
        paleo::data::DataService writer;QVERIFY(writer.openProject(modelRoot(f).toStdString(),error));
        QVERIFY(!writer.commitSourceRelocation(*prepared,error));std::string original;
        QVERIFY(writer.sourcePath(record.ref.sourceId,original,error));QCOMPARE(QString::fromUtf8(original),record.path);
        QVERIFY(!reader.prepareSourceRelocation(record.ref.sourceId,candidate.toStdString(),prepared,error,[]{return true;}));QVERIFY(!prepared);
    }
    void cancellationAndSessionCloseNeverPublish() {
        Fixture f;QVERIFY(f.init());const auto record=seismic(f);QVERIFY(record.ref.valid());const auto before=f.catalog.versions().size();
        const auto candidate=f.temp.filePath("moved.sgy");QVERIFY(QFile::rename(record.path,candidate));
        auto command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=candidate;
        QVERIFY(f.session.execute(command).pending);f.session.close();QVERIFY(f.tasks.shutdown(10000));
        QCOMPARE(f.catalog.versions().size(),before);
        paleo::data::DataService service;std::string error,path;
        QVERIFY(service.openProjectReadOnly(modelRoot(f).toStdString(),error));QVERIFY(service.sourcePath(record.ref.sourceId,path,error));
        QCOMPARE(QString::fromUtf8(path),record.path);
    }
    void failedCatalogCommitLeavesRecoverableIntent() {
        Fixture f;QVERIFY(f.init());const auto record=seismic(f);QVERIFY(record.ref.valid());
        const auto candidate=f.temp.filePath("moved.sgy");QVERIFY(QFile::rename(record.path,candidate));
        const auto connection="document-relocation-fault";
        auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(f.catalog.sqliteCatalogPath());QVERIFY(db.open());
        QSqlQuery fault(db);QVERIFY(fault.exec("CREATE TRIGGER reject_source_version BEFORE INSERT ON versions BEGIN SELECT RAISE(ABORT,'injected catalog failure'); END"));
        QSignalSpy finished(&f.session,&DocumentSession::commandFinished);
        auto command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=candidate;command.operationId="recover-once";
        QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(finished.size(),1);
        auto result=qvariant_cast<SourceCommandResult>(finished.front().front());QVERIFY(!result.ok);QCOMPARE(result.code,QString("recovery_required"));
        QVERIFY(f.session.recoveryRequired());QCOMPARE(f.catalog.currentVersion(record.assetId).id,record.versionId);
        QVERIFY(fault.exec("DROP TRIGGER reject_source_version"));fault=QSqlQuery();db.close();db=QSqlDatabase();QSqlDatabase::removeDatabase(connection);
        f.session.close();QVERIFY(f.bind());QVERIFY(f.session.recoveryRequired());
        command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=candidate;command.operationId="recover-once";
        QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(finished.size(),2);
        result=qvariant_cast<SourceCommandResult>(finished.back().front());QVERIFY2(result.ok,qPrintable(result.error));
        QVERIFY(!f.session.recoveryRequired());QCOMPARE(f.catalog.currentVersion(record.assetId).path,candidate);
    }
    void shaSourceRelocationKeepsContentIdentity() {
        Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("checks.txt",{"A"}));QVERIFY(f.bind());const auto record=f.session.sources().front();
        const auto candidate=f.temp.filePath("moved.txt");QVERIFY(QFile::rename(record.path,candidate));
        auto command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=candidate;
        QSignalSpy finished(&f.session,&DocumentSession::commandFinished);QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(finished.size(),1);
        const auto result=qvariant_cast<SourceCommandResult>(finished.front().front());QVERIFY2(result.ok,qPrintable(result.error));
        QCOMPARE(f.session.source(record.ref).ref,record.ref);QCOMPARE(f.session.source(record.ref).path,candidate);
    }
    void rawSourceCanRelocateWhileDerivedVersionRemains() {
        Fixture f;QVERIFY(f.init());const auto id=f.external("original.txt","original");const auto raw=f.catalog.versionById(id);
        auto derived=raw;derived.id=f.catalog.nextVersionId();derived.versionNumber=raw.versionNumber+1;derived.stage="DERIVED";
        derived.parentVersionIds={raw.id};
        derived.sha256=QString(64,'b');derived.path=f.temp.filePath("processed.txt");QVERIFY(write(derived.path,"processed"));QVERIFY(f.catalog.addVersion(derived));QVERIFY(f.bind());
        QVERIFY(!f.catalog.versionById(derived.id).extra.value("stale").toBool());
        const auto moved=f.temp.filePath("moved.txt");QVERIFY(QFile::rename(raw.path,moved));
        auto command=f.session.command(SourceCommand::Kind::Relocate);command.source=f.session.sourceForVersion(id).ref;command.path=moved;
        QSignalSpy finished(&f.session,&DocumentSession::commandFinished);QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(finished.size(),1);
        const auto result=qvariant_cast<SourceCommandResult>(finished.front().front());QVERIFY2(result.ok,qPrintable(result.error));
        const auto next=f.catalog.versionById(result.versionId);QCOMPARE(next.stage,QString("RAW"));QCOMPARE(next.versionNumber,derived.versionNumber+1);QCOMPARE(next.path,moved);
        QCOMPARE(f.catalog.versionById(derived.id).path,derived.path);QCOMPARE(f.catalog.versionById(raw.id).path,raw.path);
        QVERIFY(!f.catalog.versionById(derived.id).extra.value("stale").toBool());
        f.session.close();QVERIFY(f.catalog.open(f.project,&f.error));QVERIFY(f.bind());
        QCOMPARE(f.catalog.versionById(derived.id).parentVersionIds,QStringList{raw.id});
        QVERIFY(!f.catalog.versionById(derived.id).extra.value("stale").toBool());
    }
    void completedCatalogCommitCanFinishOffline_data() {
        QTest::addColumn<bool>("nativeModel");QTest::newRow("area-model")<<false;QTest::newRow("native-model")<<true;
    }
    void completedCatalogCommitCanFinishOffline() {
        QFETCH(bool,nativeModel);Fixture area;NativeFixture native;
        DataCatalog *catalog=nullptr;DocumentSession *session=nullptr;SourceRecord record;QString meta,documentId,project;
        if(nativeModel) {
            QVERIFY2(native.init(),qPrintable(native.error));catalog=native.imports.catalog();session=&native.session;
            record=session->sourceForVersion(native.original.id);meta=native.meta;documentId="native-regression";project=native.project;
        } else {
            QVERIFY(area.init());record=seismic(area);catalog=&area.catalog;session=&area.session;
            meta=area.meta;documentId="fixture-document";project=area.project;
        }
        QVERIFY(record.ref.valid());QString error;
        {DocumentSourceStore store(meta);QVERIFY2(store.ensureIdentity(documentId,&error),qPrintable(error));}
        const auto candidate=QFileInfo(record.path).absolutePath()+"/offline-recovery."+(nativeModel?"las":"sgy");
        QVERIFY(QFile::rename(record.path,candidate));const QString connection="document-journal-cleanup-fault";
        auto rebind=[&]{
            session->close();
            if(nativeModel)native.imports.setProjectDir(project);else if(!catalog->open(project,&error))return false;
            return session->bind(catalog,meta,documentId,false,&error);
        };
        auto replay=[&]{auto command=session->command(SourceCommand::Kind::Relocate);command.source=record.ref;
            command.path=candidate;command.operationId="cleanup-recovery";return session->execute(command);};
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(meta);QVERIFY(db.open());QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER reject_source_cleanup BEFORE DELETE ON document_source_operations BEGIN SELECT RAISE(ABORT,'injected cleanup failure'); END"));
            QSignalSpy finished(session,&DocumentSession::commandFinished);QVERIFY(replay().pending);QTRY_COMPARE(finished.size(),1);
            const auto failed=qvariant_cast<SourceCommandResult>(finished.front().front());QVERIFY(!failed.ok);QCOMPARE(failed.code,QString("recovery_required"));
            QVERIFY(session->recoveryRequired());QVERIFY(!failed.versionId.isEmpty());QCOMPARE(catalog->versionById(failed.versionId).path,candidate);
            const auto count=catalog->versions().size();
            QVERIFY(query.exec("SELECT payload FROM document_source_operations WHERE id='cleanup-recovery'"));QVERIFY(query.next());
            const auto intent=QJsonDocument::fromJson(query.value(0).toByteArray()).object();query.finish();QVERIFY(!intent.isEmpty());
            // A second cleanup failure must preserve the intent without doing source I/O.
            QVERIFY(QFile::remove(candidate));QVERIFY(rebind());const auto stillPending=replay();
            QVERIFY(!stillPending.ok);QVERIFY(!stillPending.pending);QCOMPARE(stillPending.code,QString("recovery_required"));QVERIFY(session->recoveryRequired());
            QVERIFY(query.exec("DROP TRIGGER reject_source_cleanup"));
            const auto setIntent=[&](const QJsonObject &value){query.prepare("UPDATE document_source_operations SET payload=? WHERE id='cleanup-recovery'");
                query.addBindValue(QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)));const bool ok=query.exec();query.finish();return ok;};
            QVector<QJsonObject> conflicts;auto changed=intent;changed["parentVersionId"]=failed.versionId;conflicts.append(changed);
            changed=intent;changed["assetId"]="another-asset";conflicts.append(changed);
            changed=intent;changed["newPath"]=candidate+".other";conflicts.append(changed);
            changed=intent;auto other=record.ref;other.fileRevisionId=QString(64,'e');changed["source"]=sourceRefJson(other);conflicts.append(changed);
            changed=intent;changed["schema"]=2;conflicts.append(changed);
            for(const auto &conflict:conflicts) {
                QVERIFY(setIntent(conflict));QVERIFY(rebind());const auto rejected=replay();
                QVERIFY(!rejected.ok);QVERIFY(!rejected.pending);QVERIFY(session->recoveryRequired());QCOMPARE(catalog->versions().size(),count);
            }
            QVERIFY(setIntent(intent));QVERIFY(rebind());
            const auto request=catalog->versionById(failed.versionId).extra.value("documentSourceRequest").toMap();
            auto wrongRequest=request;wrongRequest["newPath"]=candidate+".other";
            QVERIFY(catalog->updateVersionExtra(failed.versionId,"documentSourceRequest",wrongRequest,&error));
            const auto wrongCommit=replay();QVERIFY(!wrongCommit.ok);QVERIFY(!wrongCommit.pending);QVERIFY(session->recoveryRequired());
            QVERIFY(catalog->updateVersionExtra(failed.versionId,"documentSourceRequest",request,&error));QVERIFY(rebind());
            const auto roots=QDir(project+"/artifacts/metadata/native-relocations").entryList(QDir::Dirs|QDir::NoDotAndDotDot);
            const auto recovered=replay();QVERIFY2(recovered.ok,qPrintable(recovered.error));QVERIFY(!recovered.pending);
            QCOMPARE(recovered.code,QString("already_committed"));QCOMPARE(recovered.versionId,failed.versionId);
            QVERIFY(!session->recoveryRequired());QCOMPARE(catalog->versions().size(),count);QVERIFY(!QFileInfo::exists(candidate));
            QCOMPARE(QDir(project+"/artifacts/metadata/native-relocations").entryList(QDir::Dirs|QDir::NoDotAndDotDot),roots);
            QVERIFY(rebind());QVERIFY(!session->recoveryRequired());
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void completedOperationRejectsDifferentInputAndReplaysSameInput() {
        Fixture f;QVERIFY(f.init());const auto record=seismic(f);QVERIFY(record.ref.valid());
        const auto one=f.temp.filePath("one.sgy"),two=f.temp.filePath("two.sgy");QVERIFY(QFile::rename(record.path,one));
        auto command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=one;command.operationId="stable-operation";
        QSignalSpy finished(&f.session,&DocumentSession::commandFinished);QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(finished.size(),1);
        const auto first=qvariant_cast<SourceCommandResult>(finished.front().front());QVERIFY2(first.ok,qPrintable(first.error));const auto count=f.catalog.versions().size();
        command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=one;command.operationId="stable-operation";
        const auto replay=f.session.execute(command);QVERIFY(replay.ok);QVERIFY(!replay.pending);QCOMPARE(replay.versionId,first.versionId);QCOMPARE(f.catalog.versions().size(),count);
        QVERIFY(QFile::rename(one,two));command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=two;command.operationId="stable-operation";
        const auto conflict=f.session.execute(command);QVERIFY(!conflict.ok);QVERIFY(!conflict.pending);QCOMPARE(conflict.code,QString("operation_conflict"));
        QCOMPARE(f.catalog.currentVersion(record.assetId).path,one);QVERIFY(!f.session.recoveryRequired());
        paleo::data::DataService reader;std::string error,path;QVERIFY(reader.openProjectReadOnly(modelRoot(f).toStdString(),error));
        QVERIFY(reader.sourcePath(record.ref.sourceId,path,error));QCOMPARE(QString::fromUtf8(path),one);
    }
    void nativeModelInspectAndRelocateRemainReadableAfterReopen() {
        NativeFixture f;QVERIFY2(f.init(),qPrintable(f.error));const auto originalBinding=f.original.extra.value("nativeModel").toMap();
        const auto originalHash=f.databaseHash(originalBinding);QVERIFY(!originalHash.isEmpty());const auto record=f.session.sourceForVersion(f.original.id);
        QCOMPARE(record.modelDirectory,QFileInfo(QDir(f.project).filePath(originalBinding.value("path").toString())).absolutePath());
        auto command=f.session.command(SourceCommand::Kind::Inspect);command.source=record.ref;
        QSignalSpy observed(&f.session,&DocumentSession::sourceObserved);QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(observed.size(),1);
        QCOMPARE(qvariant_cast<SourceState>(observed.front().at(1)).model,ModelAvailability::Ready);
        const auto moved=f.temp.filePath("moved.las");QVERIFY(QFile::rename(f.path,moved));
        command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=moved;
        QSignalSpy finished(&f.session,&DocumentSession::commandFinished);QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(finished.size(),1);
        const auto result=qvariant_cast<SourceCommandResult>(finished.front().front());QVERIFY2(result.ok,qPrintable(result.error));
        QCOMPARE(result.state.bytesRead,QFileInfo(moved).size());const auto next=f.imports.catalog()->versionById(result.versionId);
        const auto newBinding=next.extra.value("nativeModel").toMap();QVERIFY(newBinding.value("path")!=originalBinding.value("path"));
        QCOMPARE(newBinding.value("storeIdentity").toString(),record.ref.modelStoreId);QCOMPARE(f.databaseHash(originalBinding),originalHash);
        QCOMPARE(f.session.sourceForVersion(next.id).ref,record.ref);
        f.session.close();f.imports.setProjectDir(f.project);QVERIFY(f.session.bind(f.imports.catalog(),f.meta,"native-regression",false,&f.error));
        LasHeaderInfo header;QList<LasCurve> curves;QVERIFY2(WellLogRead::parse(moved,header,curves,&f.error),qPrintable(f.error));
        QCOMPARE(curves.size(),2);QCOMPARE(curves[1].values[0],-999.0);QVERIFY(std::isnan(curves[1].values[1]));QCOMPARE(curves[1].values[2],-20.0);
        QCOMPARE(f.databaseHash(originalBinding),originalHash);
    }
    void nativeSnapshotSurvivesCatalogFailureAndRecovery() {
        NativeFixture f;QVERIFY2(f.init(),qPrintable(f.error));const auto record=f.session.sourceForVersion(f.original.id);
        const auto originalBinding=f.original.extra.value("nativeModel").toMap();const auto oldHash=f.databaseHash(originalBinding);
        const auto moved=f.temp.filePath("moved.las");QVERIFY(QFile::rename(f.path,moved));const QString connection="native-relocation-fault";
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(f.imports.catalog()->sqliteCatalogPath());QVERIFY(db.open());QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TRIGGER reject_native_move BEFORE INSERT ON versions BEGIN SELECT RAISE(ABORT,'native injected failure'); END"));
            auto command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=moved;command.operationId="native-recovery";
            QSignalSpy finished(&f.session,&DocumentSession::commandFinished);QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(finished.size(),1);
            const auto failed=qvariant_cast<SourceCommandResult>(finished.front().front());QVERIFY(!failed.ok);QVERIFY(f.session.recoveryRequired());
            QCOMPARE(f.imports.catalog()->currentVersion(record.assetId).id,f.original.id);QCOMPARE(f.databaseHash(originalBinding),oldHash);
            QVERIFY(query.exec("DROP TRIGGER reject_native_move"));
        }
        QSqlDatabase::removeDatabase(connection);
        const auto roots=QDir(f.project+"/artifacts/metadata/native-relocations").entryList(QDir::Dirs|QDir::NoDotAndDotDot);
        QCOMPARE(roots.size(),1);f.session.close();f.imports.setProjectDir(f.project);QVERIFY(f.session.bind(f.imports.catalog(),f.meta,"native-regression",false,&f.error));
        QVERIFY(f.session.recoveryRequired());auto command=f.session.command(SourceCommand::Kind::Relocate);command.source=record.ref;command.path=moved;command.operationId="native-recovery";
        QSignalSpy finished(&f.session,&DocumentSession::commandFinished);QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(finished.size(),1);
        const auto recovered=qvariant_cast<SourceCommandResult>(finished.front().front());QVERIFY2(recovered.ok,qPrintable(recovered.error));QVERIFY(!f.session.recoveryRequired());
        QCOMPARE(QDir(f.project+"/artifacts/metadata/native-relocations").entryList(QDir::Dirs|QDir::NoDotAndDotDot),roots);
        f.session.close();f.imports.setProjectDir(f.project);LasHeaderInfo header;QList<LasCurve> curves;
        QVERIFY2(WellLogRead::parse(moved,header,curves,&f.error),qPrintable(f.error));QCOMPARE(f.databaseHash(originalBinding),oldHash);
    }
};
QTEST_GUILESS_MAIN(TestDocumentRelocation)
#include "tst_document_relocation.moc"
