#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "services/documentsourceprobe.h"
#include <QSqlDatabase>
#include <QSqlQuery>

using namespace document_fixture;
class TestDocumentRelocation:public QObject {
    Q_OBJECT
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
        QVERIFY(fault.exec("DROP TRIGGER reject_source_version"));fault.finish();db.close();db=QSqlDatabase();QSqlDatabase::removeDatabase(connection);
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
};
QTEST_GUILESS_MAIN(TestDocumentRelocation)
#include "tst_document_relocation.moc"
