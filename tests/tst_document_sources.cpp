#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "metadata/documentsourcestore.h"
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>

using namespace document_fixture;
class TestDocumentSources:public QObject {
    Q_OBJECT
private slots:
    void referencesAndLocatorsAreScopedAndRoundTrip() {
        SourceRef a{"doc","area-model",9007199254740993LL,"quick:"+QString(64,'a')},b=a;
        QVERIFY(a.valid());QVERIFY(sourceRefFromJson(sourceRefJson(a),b));QCOMPARE(a,b);
        b.modelStoreId="another-model";QVERIFY(a.key()!=b.key());b=a;b.documentId="another-doc";QVERIFY(a.key()!=b.key());
        SourceBinding binding;binding.id="binding";binding.resourceKind="well";binding.resourceId="well-1";binding.source=a;binding.role="time_depth";
        binding.locator={LocatorKind::Worksheet,{{"sheet","测井曲线"},{"rowFirst",2}}};binding.evidence="raw sheet";
        QString error;QVERIFY2(binding.valid(&error),qPrintable(error));SourceBinding restored;
        QVERIFY(sourceBindingFromJson(sourceBindingJson(binding),restored,&error));QCOMPARE(restored,binding);
        auto future=sourceBindingJson(binding);future["schema"]=2;QVERIFY(!sourceBindingFromJson(future,restored,&error));
        QVERIFY(!SourceFingerprint::fromStored("quick:invalid").valid());
    }
    void sameWellRetainsBothSourcesAndSelectionPersists() {
        Fixture f;QVERIFY(f.init());QVERIFY2(f.preparedWellSource("first.txt",{"A","B"}),qPrintable(f.error));
        QVERIFY2(f.preparedWellSource("second.txt",{"A"}),qPrintable(f.error));QVERIFY(f.bind());
        const auto well=f.catalog.wellsMatchingName("A").front();auto values=f.session.bindings("well",well,"time_depth");
        QCOMPARE(values.size(),2);QCOMPARE(f.session.bindings("well",f.catalog.wellsMatchingName("B").front(),"time_depth").size(),1);
        auto chosen=values[0].primary?values[1]:values[0];
        auto command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId=chosen.id;
        QSignalSpy changes(&f.session,&DocumentSession::sourcesChanged);const auto result=f.session.execute(command);QVERIFY2(result.ok,qPrintable(result.error));
        QCOMPARE(changes.size(),1);values=f.session.bindings("well",well,"time_depth");
        QCOMPARE(std::count_if(values.begin(),values.end(),[](const auto &v){return v.primary;}),1);
        QVERIFY(std::any_of(values.begin(),values.end(),[&](const auto &v){return v.id==chosen.id && v.primary;}));
        const auto links=f.catalog.linksForEntity(well);
        QVERIFY(std::any_of(links.begin(),links.end(),[&](const auto &link){return link.role=="time_depth" && link.isPrimary && link.assetId==f.session.source(chosen.source).assetId;}));
        f.session.close();QVERIFY(f.bind());values=f.session.bindings("well",well,"time_depth");
        QVERIFY(std::any_of(values.begin(),values.end(),[&](const auto &v){return v.id==chosen.id && v.primary;}));
        QVERIFY(!f.session.execute(command).ok); // A command from the old session cannot mutate the new one.
    }
    void identityConflictNeedsExplicitResolution() {
        Fixture f;QVERIFY(f.init());const auto version=f.external("report.xls","fixture");QVERIFY(!version.isEmpty());
        CatalogEntity well;well.id="well-1";well.entityType="well";well.name="A";QVERIFY(f.catalog.addEntity(well));QVERIFY(f.bind());
        auto command=f.session.command(SourceCommand::Kind::RegisterBinding);
        command.binding.id="conflict";command.binding.resourceKind="well";command.binding.resourceId=well.id;command.binding.role="core";
        command.binding.source=f.session.sourceForVersion(version).ref;
        command.binding.identityClaims={{"directory","HZ19-6-12"},{"worksheet","HZ19-6-1d"}};
        QVERIFY(f.session.execute(command).ok);auto values=f.session.bindings("well",well.id);QCOMPARE(values.size(),1);
        QCOMPARE(values.front().decision,BindingDecision::Conflict);QVERIFY(!values.front().primary);
        command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId="conflict";QVERIFY(!f.session.execute(command).ok);
        command=f.session.command(SourceCommand::Kind::ConfirmBinding);command.bindingId="conflict";QVERIFY(!f.session.execute(command).ok);
        command.note="Checked the conflicting source declarations explicitly";QVERIFY(f.session.execute(command).ok);
        values=f.session.bindings("well",well.id);QCOMPARE(values.front().identityClaims.size(),2);QCOMPARE(values.front().decision,BindingDecision::Confirmed);
    }
    void readOnlyQueriesDoNotCreateStateAndRejectCommands() {
        Fixture f;QVERIFY(f.init());const auto version=f.external("raw.txt","data");QVERIFY(f.bind(true));
        QVERIFY(!QFileInfo::exists(f.meta));QCOMPARE(f.session.sources().size(),1);QVERIFY(!QFileInfo::exists(f.meta));
        auto command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding.source=f.session.sourceForVersion(version).ref;
        QCOMPARE(f.session.execute(command).code,QString("read_only"));QVERIFY(!QFileInfo::exists(f.meta));
    }
    void storeFailureDoesNotPublishOrAdvanceRevision() {
        Fixture f;QVERIFY(f.init());const auto version=f.external("raw.txt","data");
        QVERIFY(f.bind());QVERIFY(QDir().mkpath(f.meta)); // Fail only the later command publication.
        auto command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding.id="new";command.binding.resourceKind="asset";
        command.binding.resourceId="resource";command.binding.role="reference";command.binding.source=f.session.sourceForVersion(version).ref;
        QSignalSpy changes(&f.session,&DocumentSession::sourcesChanged);QVERIFY(!f.session.execute(command).ok);
        QCOMPARE(f.session.revision(),quint64(f.catalog.catalogRevision()));QCOMPARE(changes.size(),0);QVERIFY(f.session.bindings("asset","resource").isEmpty());
    }
    void modelCanBeReadWhenOriginalIsOffline() {
        Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("source.txt",{"A"}));QVERIFY(f.bind());const auto record=f.session.sources().front();
        QVERIFY(QFile::remove(record.path));auto command=f.session.command(SourceCommand::Kind::Inspect);command.source=record.ref;
        QSignalSpy observed(&f.session,&DocumentSession::sourceObserved);QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(observed.size(),1);
        const auto state=qvariant_cast<SourceState>(observed.front().at(1));QCOMPARE(state.file,FileAvailability::Missing);QCOMPARE(state.model,ModelAvailability::Ready);
    }
    void changedFileRevisionDoesNotSilentlyRetargetSelection() {
        Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("checks.txt",{"A"}));QVERIFY(f.bind());
        const auto well=f.catalog.wellsMatchingName("A").front();const auto selected=f.session.bindings("well",well,"time_depth").front();
        auto command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId=selected.id;QVERIFY(f.session.execute(command).ok);
        auto version=f.catalog.currentVersion(f.session.source(selected.source).assetId);version.id=f.catalog.nextVersionId();++version.versionNumber;
        version.sha256=QString(64,'a');version.path=f.temp.filePath("different-content.txt");QVERIFY(f.catalog.addVersion(version));
        const auto values=f.session.bindings("well",well,"time_depth");QCOMPARE(values.size(),1);
        QCOMPARE(values.front().source,selected.source);QVERIFY(!values.front().primary);QCOMPARE(values.front().decision,BindingDecision::Unresolved);
        command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId=selected.id;QVERIFY(!f.session.execute(command).ok);
    }
    void failedBindingTransactionRollsBackLegacyPrimary() {
        Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("a.txt",{"A"}));QVERIFY(f.preparedWellSource("b.txt",{"A"}));QVERIFY(f.bind());
        const auto well=f.catalog.wellsMatchingName("A").front();const auto before=f.session.bindings("well",well,"time_depth");
        const auto chosen=before[0].primary?before[1]:before[0];const auto revision=f.session.revision();
        const QString connection="source-binding-fault";
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(f.catalog.sqliteCatalogPath());QVERIFY(db.open());
            QSqlQuery query(db);QVERIFY(query.exec("CREATE TRIGGER reject_source_binding BEFORE UPDATE ON versions BEGIN SELECT RAISE(ABORT,'injected binding failure'); END"));
            auto command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId=chosen.id;
            QSignalSpy changes(&f.session,&DocumentSession::sourcesChanged);QVERIFY(!f.session.execute(command).ok);
            QCOMPARE(changes.size(),0);QCOMPARE(f.session.revision(),revision);
            QCOMPARE(f.session.bindings("well",well,"time_depth"),before);
            for(const auto &link:f.catalog.linksForEntity(well))
                if(link.assetId==f.session.source(chosen.source).assetId && link.role=="time_depth")QVERIFY(!link.isPrimary);
            QVERIFY(query.exec("DROP TRIGGER reject_source_binding"));
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void locatorSelectionIsScopedAndDoesNotChangeFilePrimary() {
        Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("checks.txt",{"A"}));QVERIFY(f.bind());
        const auto well=f.catalog.wellsMatchingName("A").front();const auto original=f.session.bindings("well",well,"time_depth").front();
        for(const auto &scope:QStringList{"column","map"})for(int i=0;i<2;++i) {
            auto command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=original;
            command.binding.id=scope+QString::number(i);command.binding.scope=scope;
            command.binding.locator={LocatorKind::Worksheet,{{"sheet",QString::number(i)}}};QVERIFY(f.session.execute(command).ok);
            command=f.session.command(SourceCommand::Kind::ConfirmBinding);command.bindingId=scope+QString::number(i);QVERIFY(f.session.execute(command).ok);
        }
        auto command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId="column1";QVERIFY(f.session.execute(command).ok);
        command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId="map0";QVERIFY(f.session.execute(command).ok);
        for(const auto &scope:QStringList{"column","map"}) {
            const auto values=f.session.bindings("well",well,"time_depth",scope);
            QCOMPARE(std::count_if(values.begin(),values.end(),[](const auto &v){return v.primary;}),1);
        }
        QVERIFY(f.session.bindings("well",well,"time_depth").front().primary);
    }
    void removeBindingAlsoUnresolvesLegacyLink() {
        Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("checks.txt",{"A"}));QVERIFY(f.bind());
        const auto well=f.catalog.wellsMatchingName("A").front();const auto binding=f.session.bindings("well",well,"time_depth").front();
        auto command=f.session.command(SourceCommand::Kind::RemoveBinding);command.bindingId=binding.id;QVERIFY(f.session.execute(command).ok);
        QVERIFY(f.session.bindings("well",well,"time_depth").isEmpty());QVERIFY(f.catalog.linksForEntity(well).isEmpty());
        const auto rejected=f.session.bindings("well",well,"time_depth",{},true);
        QCOMPARE(rejected.size(),1);QCOMPARE(rejected.front().decision,BindingDecision::Rejected);QVERIFY(!rejected.front().primary);
        QVERIFY(f.catalog.hasEntity(well));QVERIFY(f.session.source(binding.source).ref.valid());
    }
    void oversizedPagedHeaderFailsWithinPrefixBudget() {
        Fixture f;QVERIFY(f.init());QByteArray prefix(16,'\0');std::memcpy(prefix.data(),"SF3PWG01",8);
        qToLittleEndian<quint32>(7,prefix.data()+8);qToLittleEndian<quint32>(0x7ffff000,prefix.data()+12);
        const auto version=f.external("oversized.sf3p",prefix);QVERIFY(!version.isEmpty());QVERIFY(f.bind());
        auto command=f.session.command(SourceCommand::Kind::Inspect);command.source=f.session.sourceForVersion(version).ref;
        QSignalSpy observed(&f.session,&DocumentSession::sourceObserved);QVERIFY(f.session.execute(command).pending);QTRY_COMPARE(observed.size(),1);
        const auto state=qvariant_cast<SourceState>(observed.front().at(1));QCOMPARE(state.code,QString("paged_header_budget"));
        QCOMPARE(state.derivative,DerivativeState::Failed);QCOMPARE(state.bytesRead,qint64(16));QVERIFY(!state.payloadVerified);
    }
};
QTEST_GUILESS_MAIN(TestDocumentSources)
#include "tst_document_sources.moc"
