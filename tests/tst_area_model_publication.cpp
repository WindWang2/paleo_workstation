#include <QtTest>
#include "io/areamodel.h"
#include "io/areamodelpublication.h"
#include <paleo_data/service.h>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <future>
#include <thread>

namespace {
using namespace paleo::data;
struct Fixture {
    QTemporaryDir temp;
    QString project=temp.path()+"/project", staging=temp.path()+"/.paleo-area-fixture";
    ModelChangeHead expected,target;
    std::string error;
    QString active() const {return paleo::area_model::directory(project);}
    QString prepared() const {return paleo::area_model::directory(staging);}
    QString intent() const {return active()+".publication.json";}
    bool prepare(bool managedSource=false) {
        DataService base;
        if (!base.createProject(active().toStdString(),"fixture",error)) return false;
        WellInfo well; well.wellName="original"; long long id=0;
        if (!base.createWell(well,id,error)) return false;
        if(managedSource) {
            QFile source(temp.path()+"/managed.las");if(!source.open(QIODevice::WriteOnly))return false;
            source.write("~Version\nVERS. 2.0\nWRAP. NO\n~Well\nWELL. Managed\nNULL. -999.25\n~Curve\nDEPT.M\nGR.API\n~ASCII\n100 12\n101 13\n");source.close();
            ImportReport report;if(!base.importFile(source.fileName().toStdString(),{},report,error))return false;
        }
        if(!base.readModelChangeHead(expected,error))return false;
        QDir().mkpath(prepared());
        if (!base.snapshotDatabase((prepared()+"/data.sqlite").toStdString(),error)) return false;
        base.closeProject(); DataService draft;
        if (!draft.openProject(prepared().toStdString(),error)) return false;
        well.wellName="prepared";
        ModelChangeHead head;
        return draft.createWell(well,id,error) && draft.readModelChangeHead(head,error) &&
            draft.forkModelGeneration(head,target,error);
    }
    QJsonObject record() const {
        QFile file(intent()); if (!file.open(QIODevice::ReadOnly)) return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }
    int wellCount() {
        DataService reader; std::vector<WellInfoRecord> wells;
        if (!reader.openProjectReadOnly(active().toStdString(),error) || !reader.listWellInfos(wells,error)) return -1;
        return int(wells.size());
    }
};
bool acknowledge(QString *) {return true;}
}

class AreaModelPublicationTest : public QObject {
    Q_OBJECT
private slots:
    void rejectsUnrelatedPreparedStore() {
        Fixture f;QVERIFY(f.prepare());const auto foreign=f.temp.path()+"/.paleo-area-foreign";
        DataService service;QVERIFY(service.createProject(paleo::area_model::directory(foreign).toStdString(),"foreign",f.error));service.closeProject();
        QString error;QVERIFY(!paleo::area_model::publishPreparedModel(f.project,foreign,f.expected,&error));
        QVERIFY(error.contains("ModelStoreConflict"));QVERIFY(!QFileInfo::exists(f.intent()));QCOMPARE(f.wellCount(),1);
    }
    void databaseOnlySnapshotCannotLoseManagedOriginal() {
        Fixture f;QVERIFY2(f.prepare(true),f.error.c_str());QString error;
        QVERIFY(!paleo::area_model::publishPreparedModel(f.project,f.staging,f.expected,&error));
        QVERIFY(error.contains(QStringLiteral("受管原件")));QVERIFY(!QFileInfo::exists(f.intent()));
        DataService reader;QVERIFY(reader.openProjectReadOnly(f.active().toStdString(),f.error));
        std::vector<SourceIndexEntry> sources;QVERIFY(reader.listSourceIndexEntries(sources,f.error));QVERIFY(!sources.empty());
        QVERIFY(QFileInfo::exists(QDir(f.active()).filePath(QString::fromStdString(sources.front().storedPath))));
    }
    void rejectsConcurrentEdit() {
        Fixture f; QVERIFY2(f.prepare(),f.error.c_str());
        DataService edit; QVERIFY(edit.openProject(f.active().toStdString(),f.error));
        WellInfo well; well.wellName="concurrent"; long long id=0;
        QVERIFY(edit.createWell(well,id,f.error)); edit.closeProject();
        QString error;
        QVERIFY(!paleo::area_model::publishPreparedModel(f.project,f.staging,f.expected,&error));
        QVERIFY(error.contains("ModelHeadConflict")); QCOMPARE(f.wellCount(),2);
        QVERIFY(!QFileInfo::exists(f.intent())); QVERIFY(QFileInfo::exists(f.prepared()+"/data.sqlite"));
    }
    void previouslyOpenedWriterCannotModifyRetainedGeneration() {
        Fixture f;QVERIFY2(f.prepare(),f.error.c_str());DataService old;
        QVERIFY2(old.openProject(f.active().toStdString(),f.error),f.error.c_str());QString error;
        const bool published=paleo::area_model::publishPreparedModel(f.project,f.staging,f.expected,&error);
        if(published) {
            WellInfo well;well.wellName="must-not-write-backup";long long id=0;
            QVERIFY(!old.createWell(well,id,f.error));QVERIFY(f.error.find("StaleModelGeneration")!=std::string::npos);
        }
        old.closeProject();
        QVERIFY2(paleo::area_model::recoverModelPublication(f.project,acknowledge,&error),qPrintable(error));
        QCOMPARE(f.wellCount(),2);
    }
    void recoversProjectionWithoutSources() {
        Fixture f; QVERIFY2(f.prepare(),f.error.c_str()); QString error;
        QVERIFY2(paleo::area_model::publishPreparedModel(f.project,f.staging,f.expected,&error),qPrintable(error));
        const auto backup=f.record()["backup"].toString();
        QVERIFY(!paleo::area_model::recoverModelPublication(f.project,[](QString *detail) {
            *detail="injected projection failure"; return false;
        },&error));
        QVERIFY(error.contains(QStringLiteral("模型已提交"))); QCOMPARE(f.wellCount(),2);
        QVERIFY(QFileInfo::exists(backup+"/data.sqlite")); QVERIFY(QFileInfo::exists(f.intent()));
        QVERIFY2(paleo::area_model::recoverModelPublication(f.project,acknowledge,&error),qPrintable(error));
        QVERIFY(!QFileInfo::exists(f.intent())); QVERIFY(QFileInfo::exists(backup+"/data.sqlite"));
        QVERIFY(QFileInfo::exists(backup+".retained.json")); QCOMPARE(f.wellCount(),2);
        QVERIFY(paleo::area_model::recoverModelPublication(f.project,acknowledge,&error));
    }
    void resumesAfterOldDirectoryMoved() {
        Fixture f; QVERIFY2(f.prepare(),f.error.c_str()); QString error;
        QVERIFY(paleo::area_model::publishPreparedModel(f.project,f.staging,f.expected,&error));
        QVERIFY(QDir().rename(f.active(),f.prepared())); // Simulate crash after old -> backup.
        QVERIFY(!QFileInfo::exists(f.active()+"/data.sqlite"));
        QVERIFY2(paleo::area_model::recoverModelPublication(f.project,acknowledge,&error),qPrintable(error));
        QCOMPARE(f.wellCount(),2);
    }
    void resumesBeforeDirectorySwap() {
        Fixture f; QVERIFY2(f.prepare(),f.error.c_str()); QString error;
        QVERIFY(paleo::area_model::publishPreparedModel(f.project,f.staging,f.expected,&error));
        const auto backup=f.record()["backup"].toString();
        QVERIFY(QDir().rename(f.active(),f.prepared())); QVERIFY(QDir().rename(backup,f.active()));
        QCOMPARE(f.wellCount(),1);
        QVERIFY2(paleo::area_model::recoverModelPublication(f.project,acknowledge,&error),qPrintable(error));
        QCOMPARE(f.wellCount(),2);
    }
    void refusesCorruptRecoveryPaths() {
        Fixture f; QVERIFY2(f.prepare(),f.error.c_str()); QString error;
        QVERIFY(paleo::area_model::publishPreparedModel(f.project,f.staging,f.expected,&error));
        auto record=f.record(); record["backup"]=f.temp.path();
        QFile file(f.intent()); QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));
        file.write(QJsonDocument(record).toJson()); file.close();
        QVERIFY(!paleo::area_model::recoverModelPublication(f.project,acknowledge,&error));
        QVERIFY(QFileInfo::exists(f.intent())); QCOMPARE(f.wellCount(),2);
    }
    void honorsOtherWriterLease() {
        Fixture f; QVERIFY2(f.prepare(),f.error.c_str()); QString error;
        std::promise<bool> ready; std::promise<void> release;
        auto released=release.get_future();
        std::thread writer([&] {
            std::string detail;
            auto lease=tryAcquireModelWriteLease(f.active().toStdString(),0,detail);
            ready.set_value(bool(lease)); released.wait();
        });
        const bool acquired=ready.get_future().get();
        const bool published=paleo::area_model::publishPreparedModel(f.project,f.staging,f.expected,&error);
        release.set_value(); writer.join();
        QVERIFY(acquired); QVERIFY(!published); QVERIFY(!QFileInfo::exists(f.intent()));
        QCOMPARE(f.wellCount(),1);
    }
    void honorsPreparedWriterLease() {
        Fixture f;QVERIFY2(f.prepare(),f.error.c_str());QString error;
        std::promise<bool> ready;std::promise<void> release;auto released=release.get_future();
        std::thread writer([&] {
            std::string detail;auto lease=tryAcquireModelWriteLease(f.prepared().toStdString(),0,detail);
            ready.set_value(bool(lease));released.wait();
        });
        const bool acquired=ready.get_future().get();
        const bool published=paleo::area_model::publishPreparedModel(f.project,f.staging,f.expected,&error);
        release.set_value();writer.join();QVERIFY(acquired);QVERIFY(!published);
        QVERIFY(!QFileInfo::exists(f.intent()));QCOMPARE(f.wellCount(),1);
    }
    void preservesPostPublicationEdits() {
        Fixture f; QVERIFY2(f.prepare(),f.error.c_str()); QString error;
        QVERIFY(paleo::area_model::publishPreparedModel(f.project,f.staging,f.expected,&error));
        DataService edit; QVERIFY(edit.openProject(f.active().toStdString(),f.error));
        WellInfo well; well.wellName="later"; long long id=0;
        QVERIFY(edit.createWell(well,id,f.error)); edit.closeProject();
        QVERIFY2(paleo::area_model::recoverModelPublication(f.project,acknowledge,&error),qPrintable(error));
        QCOMPARE(f.wellCount(),3);
    }
    void preparedChangesDoNotReplaceOriginal() {
        Fixture f; QVERIFY2(f.prepare(),f.error.c_str()); QString error;
        QVERIFY(paleo::area_model::publishPreparedModel(f.project,f.staging,f.expected,&error));
        const auto backup=f.record()["backup"].toString();
        QVERIFY(QDir().rename(f.active(),f.prepared())); QVERIFY(QDir().rename(backup,f.active()));
        DataService edit; QVERIFY(edit.openProject(f.prepared().toStdString(),f.error));
        WellInfo well; well.wellName="changed draft"; long long id=0;
        QVERIFY(edit.createWell(well,id,f.error)); edit.closeProject();
        QVERIFY(!paleo::area_model::recoverModelPublication(f.project,acknowledge,&error));
        QCOMPARE(f.wellCount(),1); QVERIFY(QFileInfo::exists(f.intent()));
    }
};
QTEST_GUILESS_MAIN(AreaModelPublicationTest)
#include "tst_area_model_publication.moc"
