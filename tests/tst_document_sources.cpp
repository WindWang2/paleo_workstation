#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "metadata/documentsourcestore.h"
#include "services/timedepthdependencies.h"
#include "services/timedepthalignment.h"
#include <paleo_data/object_descriptor.h>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>

using namespace document_fixture;
class TestDocumentSources:public QObject {
    Q_OBJECT
    static SourceCommandResult runCommand(Fixture &f, const SourceCommand &command)
    {
        QSignalSpy finished(&f.session,&DocumentSession::commandFinished);
        auto result=f.session.execute(command);
        if(!result.pending)return result;
        if(finished.isEmpty() && !finished.wait(10000)) {
            result.error=QStringLiteral("relation command timed out");
            return result;
        }
        return qvariant_cast<SourceCommandResult>(finished.first().first());
    }
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
    void fileCandidatesPreservePrimaryAndDoNotOwnTheLegacyLink() {
        Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("checks.txt",{"A"}));QVERIFY(f.bind());
        const auto well=f.catalog.wellsMatchingName("A").front();const auto original=f.session.bindings("well",well,"time_depth").front();QVERIFY(original.primary);
        for(int i=0;i<2;++i) {
            auto command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=original;command.binding.id="candidate"+QString::number(i);
            if(i)command.binding.identityClaims={{"filename","A"},{"worksheet","B"}};
            QVERIFY(f.session.execute(command).ok);const auto values=f.session.bindings("well",well,"time_depth");
            QCOMPARE(std::count_if(values.cbegin(),values.cend(),[](const auto &v){return v.primary;}),1);
            QVERIFY(std::any_of(values.cbegin(),values.cend(),[&](const auto &v){return v.id==original.id && v.primary;}));
            const auto candidate=std::find_if(values.cbegin(),values.cend(),[&](const auto &v){return v.id==command.binding.id;});QVERIFY(candidate!=values.cend());
            QCOMPARE(candidate->decision,i?BindingDecision::Conflict:BindingDecision::Candidate);QVERIFY(!candidate->primary);
        }
        auto command=f.session.command(SourceCommand::Kind::RemoveBinding);command.bindingId="candidate0";QVERIFY(f.session.execute(command).ok);
        const auto links=f.catalog.linksForEntity(well);QCOMPARE(links.size(),1);QVERIFY(links.front().isPrimary);
        const auto values=f.session.bindings("well",well,"time_depth");
        QVERIFY(std::any_of(values.cbegin(),values.cend(),[&](const auto &v){return v.id==original.id && v.primary;}));
    }
    void confirmedFileAliasesRequireExplicitSelectionAndRemainSinglePrimary() {
        Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("checks.txt",{"A"}));QVERIFY(f.bind());
        const auto well=f.catalog.wellsMatchingName("A").front();const auto original=f.session.bindings("well",well,"time_depth").front();
        for(const auto &id:QStringList{"copy-a","copy-b"}) {
            auto command=f.session.command(SourceCommand::Kind::RegisterBinding);command.binding=original;command.binding.id=id;QVERIFY(f.session.execute(command).ok);
            command=f.session.command(SourceCommand::Kind::ConfirmBinding);command.bindingId=id;QVERIFY(f.session.execute(command).ok);
        }
        auto values=f.session.bindings("well",well,"time_depth");QCOMPARE(values.size(),3);
        QVERIFY(std::any_of(values.cbegin(),values.cend(),[&](const auto &v){return v.id==original.id && v.primary;}));
        for(const auto &id:QStringList{"copy-a","copy-b"}) {
            auto command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId=id;QVERIFY(f.session.execute(command).ok);
            values=f.session.bindings("well",well,"time_depth");QCOMPARE(values.size(),2);
            QCOMPARE(std::count_if(values.cbegin(),values.cend(),[](const auto &v){return v.primary;}),1);
            QVERIFY(std::any_of(values.cbegin(),values.cend(),[&](const auto &v){return v.id==id && v.primary;}));
        }
        f.session.close();QVERIFY(f.bind());values=f.session.bindings("well",well,"time_depth");
        QCOMPARE(std::count_if(values.cbegin(),values.cend(),[](const auto &v){return v.primary;}),1);
        QVERIFY(std::any_of(values.cbegin(),values.cend(),[](const auto &v){return v.id=="copy-b" && v.primary;}));
        auto command=f.session.command(SourceCommand::Kind::RemoveBinding);command.bindingId="copy-a";QVERIFY(f.session.execute(command).ok);
        const auto links=f.catalog.linksForEntity(well);QCOMPARE(links.size(),1);QVERIFY(links.front().isPrimary);
        values=f.session.bindings("well",well,"time_depth");QCOMPARE(values.size(),1);QVERIFY(values.front().primary);
    }
    void malformedBindingContainersFailClosed_data() {
        QTest::addColumn<QVariant>("container");QTest::newRow("string")<<QVariant(QStringLiteral("broken-container"));
        QTest::newRow("array")<<QVariant(QVariantList{QString("broken-container")});
        QTest::newRow("null")<<QVariant();
    }
    void malformedBindingContainersFailClosed() {
        QFETCH(QVariant,container);Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("checks.txt",{"A"}));QVERIFY(f.bind());
        const auto record=f.session.sources().front();const auto well=f.catalog.wellsMatchingName("A").front();
        // Invalid QVariant is the public deletion sentinel; use a JSON null value
        // to exercise a present-but-invalid container instead of deleting the key.
        if(!container.isValid())container=QVariant::fromValue(nullptr);
        QVERIFY(f.catalog.updateVersionExtra(record.versionId,"documentSourceBindings",container,&f.error));
        f.session.close();QVERIFY(f.catalog.open(f.project,&f.error));QVERIFY(f.bind());
        QVERIFY(!f.session.loadError().isEmpty());QVERIFY(f.session.bindings("well",well,"time_depth").isEmpty());
        auto command=f.session.command(SourceCommand::Kind::Inspect);command.source=record.ref;
        QCOMPARE(f.session.execute(command).code,QString("source_metadata_invalid"));
    }
    void oldDuplicatePrimaryMetadataGetsOneDeterministicRepresentative() {
        Fixture f;QVERIFY(f.init());QVERIFY(f.preparedWellSource("checks.txt",{"A"}));QVERIFY(f.bind());
        const auto well=f.catalog.wellsMatchingName("A").front();const auto original=f.session.bindings("well",well,"time_depth").front();
        QVariantMap payload;
        for(const auto &id:QStringList{"copy-b","copy-a"}) {
            auto binding=original;binding.id=id;binding.primary=true;binding.revision=1;payload.insert(id,sourceBindingJson(binding).toVariantMap());
        }
        QVERIFY(f.catalog.updateVersionExtra(f.session.source(original.source).versionId,"documentSourceBindings",payload,&f.error));
        for(int i=0;i<2;++i) {
            const auto values=f.session.bindings("well",well,"time_depth");QCOMPARE(values.size(),2);
            QCOMPARE(std::count_if(values.cbegin(),values.cend(),[](const auto &v){return v.primary;}),1);
            QVERIFY(std::any_of(values.cbegin(),values.cend(),[](const auto &v){return v.id=="copy-a" && v.primary;}));
            f.session.close();QVERIFY(f.bind());
        }
    }
    void nativeModelLocationAndStableIdentityAreSeparate() {
        Fixture f;QVERIFY(f.init());const auto version=f.external("native.txt","data");
        const QString original="artifacts/raw/original/native-model/data.sqlite",relocated="artifacts/raw/relocated/native-model/data.sqlite";
        const QVariantMap native{{"schema",1},{"path",relocated},{"storeIdentity",original},{"sourceId",qlonglong(7)},
            {"sha256",QString(64,'a')},{"sourceSha256",f.catalog.versionById(version).sha256}};
        QVERIFY(f.catalog.updateVersionExtra(version,"nativeModel",native,&f.error));QVERIFY(f.bind());
        const auto record=f.session.sourceForVersion(version);QVERIFY(record.ref.valid());QVERIFY(f.session.loadError().isEmpty());
        QCOMPARE(record.ref.modelStoreId,original);
        QCOMPARE(record.modelDirectory,QDir(f.project).filePath("artifacts/raw/relocated/native-model"));
        QVERIFY(!record.modelDirectory.endsWith("data.sqlite"));
    }
    void unsafeNativeModelPathsFailBeforeSourceCommands() {
        Fixture f;QVERIFY(f.init());const auto version=f.external("native.txt","data");
        const QVariantMap native{{"path","../outside/data.sqlite"},{"sourceId",qlonglong(7)}};
        QVERIFY(f.catalog.updateVersionExtra(version,"nativeModel",native,&f.error));QVERIFY(f.bind());
        QVERIFY(!f.session.loadError().isEmpty());const auto record=f.session.sourceForVersion(version);
        QCOMPARE(record.state.model,ModelAvailability::Failed);QVERIFY(record.modelDirectory.isEmpty());
        auto command=f.session.command(SourceCommand::Kind::Inspect);command.source=record.ref;
        QCOMPARE(f.session.execute(command).code,QString("source_metadata_invalid"));
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
    void sameSourceGroupsKeepIndependentSchemeSelectionsAfterReopen() {
        Fixture f;QVERIFY(f.init());
        QVERIFY(f.preparedWellSource("multi.txt",{"A"},{"same-TDR","same-TDR"},{"same-track","same-track"}));QVERIFY(f.bind());
        const auto well=f.catalog.wellsMatchingName("A").front();const auto source=f.session.sources().front();
        const auto modelWell=f.catalog.entityById(well).extra.value("areaModelWellId").toLongLong();QVERIFY(modelWell>0);
        paleo::data::DataService reader;std::string error;
        QVERIFY(reader.openProjectReadOnly(source.modelDirectory.toStdString(),error));
        std::vector<paleo::data::DataEntry> relations,tracks;
        QVERIFY(reader.listData("time_depth",modelWell,relations,error));QCOMPARE(relations.size(),size_t(2));
        QVERIFY(reader.listData("intervals",modelWell,tracks,error));QCOMPARE(tracks.size(),size_t(2));
        QCOMPARE(relations[0].name,relations[1].name);QCOMPARE(tracks[0].name,tracks[1].name);
        paleo::data::TimeDepthTable first,second;
        QVERIFY(reader.readTimeDepth(relations[0].id,first,error));QVERIFY(reader.readTimeDepth(relations[1].id,second,error));
        QVERIFY(first.depthTimePairs.front().timeValue!=second.depthTimePairs.front().timeValue);reader.closeProject();
        const auto scopeA=QStringLiteral("scheme-A@r1/well-tie/volume-1@r1");
        const auto scopeB=QStringLiteral("scheme-B@r1/well-tie/volume-1@r1");
        const auto add=[&](const QString &scope,const QString &role,const QString &kind,qlonglong id) {
            auto command=f.session.command(SourceCommand::Kind::RegisterBinding);
            command.binding.id=scope+"/"+kind+"/"+QString::number(id);command.binding.resourceKind="well";command.binding.resourceId=well;
            command.binding.role=role;command.binding.scope=scope;command.binding.source=source.ref;
            command.binding.locator={LocatorKind::RecordGroup,{{"key",kind+"/"+QString::number(id)}}};
            if(!f.session.execute(command).ok)return false;
            const auto bindingId=command.binding.id;command=f.session.command(SourceCommand::Kind::ConfirmBinding);command.bindingId=bindingId;
            return f.session.execute(command).ok;
        };
        for(const auto &scope:{scopeA,scopeB}) {
            for(const auto &entry:relations) {QCOMPARE(entry.sourceId,source.ref.sourceId);QVERIFY(add(scope,"time_depth","time_depth",entry.id));}
            for(const auto &entry:tracks) {QCOMPARE(entry.sourceId,source.ref.sourceId);QVERIFY(add(scope,"stratigraphy","intervals",entry.id));}
        }
        for(int i=0;i<2;++i)for(const auto &role:QStringList{"time_depth","stratigraphy"}) {
            const auto scope=i==0?scopeA:scopeB;auto choices=f.session.bindings("well",well,role,scope);QCOMPARE(choices.size(),2);
            QVERIFY(std::none_of(choices.cbegin(),choices.cend(),[](const auto &v){return v.primary;}));
            auto command=f.session.command(SourceCommand::Kind::SelectPrimary);command.bindingId=choices[i].id;QVERIFY(f.session.execute(command).ok);
        }
        const auto assertSelections=[&] {
            for(const auto &role:QStringList{"time_depth","stratigraphy"}) {
                const auto a=f.session.bindings("well",well,role,scopeA),b=f.session.bindings("well",well,role,scopeB);
                QCOMPARE(a.size(),2);QCOMPARE(b.size(),2);QVERIFY(a[0].primary);QVERIFY(!a[1].primary);QVERIFY(!b[0].primary);QVERIFY(b[1].primary);
                QVERIFY(a[0].locator.fields.value("key")!=b[1].locator.fields.value("key"));QCOMPARE(a[0].source,b[1].source);
            }
            QVERIFY(f.session.bindings("well",well,"time_depth","scheme-A@r2/well-tie/volume-1@r1").isEmpty());
            QVERIFY(f.session.bindings("well",well,"time_depth","scheme-A@r1/well-tie/volume-2@r1").isEmpty());
        };
        assertSelections();f.session.close();QVERIFY(f.bind());assertSelections();
    }

    void typedTimeDepthDependenciesFollowExactSelectionAndRecoverCommittedSemantics()
    {
        Fixture f;
        QVERIFY(f.init());
        QVERIFY(f.preparedWellSource("A.txt",{"A"},{"first","second"}));
        QVERIFY(f.preparedWellSource("B.txt",{"B"}));
        QVERIFY(f.bind());
        const auto model=paleo::area_model::directory(f.project);
        paleo::data::DataService sdk;
        std::string detail;
        QVERIFY2(sdk.openProjectReadOnly(model.toStdString(),detail),detail.c_str());
        const auto useFor=[&](const QString &name,RelationPurpose purpose) {
            const auto well=f.catalog.storedEntityById(f.catalog.wellsMatchingName(name).value(0));
            InterpretationContext context;
            context.documentId=f.session.documentId();
            context.modelStoreId="area-model";
            context.purpose=purpose;
            return RelationUseSpec{context,well.id,well.extra.value("areaModelWellId").toLongLong(),RelationKind::TimeDepth};
        };
        const auto mapA=useFor("A",RelationPurpose::Mapping);
        const auto tieA=useFor("A",RelationPurpose::WellTie);
        const auto mapB=useFor("B",RelationPurpose::Mapping);
        QVERIFY(mapA.valid());
        QVERIFY(tieA.valid());
        QVERIFY(mapB.valid());
        const auto refFor=[&](const RelationUseSpec &use,size_t row) {
            std::vector<paleo::data::DataEntry> tables;
            paleo::data::DataObjectDescriptor entry;
            if(!sdk.listData("time_depth",use.modelWellId,tables,detail) || row>=tables.size() ||
               !sdk.describeDataObject(paleo::data::DataObjectKind::TimeDepth,tables[row].id,entry,detail))
                return ModelObjectRef{};
            for(const auto &source:f.session.sources())
                if(source.ref.sourceId==entry.sourceId)
                    return ModelObjectRef{source.ref,ObjectKind::TimeDepth,entry.id,QString::fromStdString(entry.revision)};
            return ModelObjectRef{};
        };
        const auto a=refFor(mapA,0),replacement=refFor(mapA,1),b=refFor(mapB,0);
        QVERIFY(a.valid());
        QVERIFY(replacement.valid());
        QVERIFY(b.valid());
        sdk.closeProject();
        const auto registerChoice=[&](const QString &id,const RelationUseSpec &use,const ModelObjectRef &ref) {
            auto command=f.session.command(SourceCommand::Kind::RegisterBinding);
            command.binding.id=id;
            if(!setRelationBinding(command.binding,use,ref,&f.error) || !runCommand(f,command).ok)
                return false;
            command=f.session.command(SourceCommand::Kind::ConfirmBinding);
            command.bindingId=id;
            return runCommand(f,command).ok;
        };
        const auto select=[&](const QString &id) {
            auto command=f.session.command(SourceCommand::Kind::SelectPrimary);
            command.bindingId=id;
            return runCommand(f,command);
        };
        QVERIFY(registerChoice("map-a",mapA,a));
        QVERIFY(registerChoice("map-a-alias",mapA,a));
        QVERIFY(registerChoice("tie-a",tieA,a));
        QVERIFY(registerChoice("map-b",mapB,b));
        QVERIFY(select("map-a").ok);
        QVERIFY(select("tie-a").ok);
        QVERIFY(select("map-b").ok);

        const auto rawA=f.session.source(a.source).versionId;
        const auto publish=[&](const QString &parent,const QVariantMap &receipt=QVariantMap{}) {
            CatalogAsset asset;
            asset.id=f.catalog.nextAssetId();
            asset.type="raster";
            asset.displayName=asset.id;
            CatalogVersion version;
            version.id=f.catalog.nextVersionId();
            version.assetId=asset.id;
            version.stage="DERIVED";
            version.parentVersionIds={parent};
            if(!receipt.isEmpty())version.extra.insert("timeDepthDependencies",QVariantList{receipt});
            return f.catalog.addAsset(asset,&f.error) && f.catalog.addVersion(version,&f.error)
                ? version.id : QString();
        };
        const auto receiptA=timeDepthDependency(f.session.relationRequest(mapA,&f.error));
        const auto receiptTie=timeDepthDependency(f.session.relationRequest(tieA,&f.error));
        const auto receiptB=timeDepthDependency(f.session.relationRequest(mapB,&f.error));
        QVERIFY(!receiptA.isEmpty());
        QVERIFY(!receiptTie.isEmpty());
        QVERIFY(!receiptB.isEmpty());
        QVERIFY2(validateTimeDepthDependencies(f.catalog,{receiptA,receiptTie,receiptB},&f.error),qPrintable(f.error));
        const auto productA=publish(rawA,receiptA),productTie=publish(rawA,receiptTie);
        const auto productB=publish(f.session.source(b.source).versionId,receiptB),child=publish(productA);
        QVERIFY(!productA.isEmpty() && !productTie.isEmpty() && !productB.isEmpty() && !child.isEmpty());
        const auto stale=[&](const QString &id){return f.catalog.versionById(id).extra.value("stale").toBool();};

        // The same numerical input selected through an equivalent binding or moved
        // file locator must keep both the receipt and its derived products current.
        QVERIFY(select("map-a-alias").ok);
        const auto oldPath=f.session.source(a.source).path;
        const auto moved=f.temp.filePath("A-moved.txt");
        QVERIFY(QFile::rename(oldPath,moved));
        auto relocate=f.session.command(SourceCommand::Kind::Relocate);
        relocate.source=a.source;
        relocate.path=moved;
        const auto relocated=runCommand(f,relocate);
        QVERIFY2(relocated.ok,qPrintable(relocated.error));
        QVERIFY(!stale(productA));
        QVERIFY2(validateTimeDepthDependencies(f.catalog,{receiptA},&f.error),qPrintable(f.error));

        const auto chooseReplacement=[&] {
            auto command=f.session.command(SourceCommand::Kind::ChooseRelation);
            command.binding.id="map-a-replacement";
            if(!setRelationBinding(command.binding,mapA,replacement,&f.error))return SourceCommandResult{};
            return runCommand(f,command);
        };
        f.catalog.debugAbortJournalAfter(1);
        const auto rejected=chooseReplacement();
        QVERIFY(!rejected.ok);
        QCOMPARE(rejected.code,QString("binding_commit_failed"));
        QVERIFY(!stale(productA));
        QVERIFY(!stale(child));
        QCOMPARE(f.session.relationView(mapA).selectedObject,a);
        QCOMPARE(f.session.relationView(mapA).choices.size(),2);
        f.catalog.debugAbortJournalAfter(0);
        const auto chosen=chooseReplacement();
        QVERIFY2(chosen.ok,qPrintable(chosen.error));
        QCOMPARE(f.session.relationView(mapA).selectedObject,replacement);
        QVERIFY(stale(productA));
        QVERIFY(stale(child));
        QVERIFY(!stale(productTie));
        QVERIFY(!stale(productB));
        QVERIFY(!stale(rawA));
        QVERIFY(!validateTimeDepthDependencies(f.catalog,{receiptA},&f.error));
        QCOMPARE(f.catalog.versionById(productA).extra.value("timeDepthDependencies").toList(),QVariantList{receiptA});
        QVERIFY(!f.catalog.updateVersionExtra(productA,"timeDepthDependencies",QVariantList{},&f.error));

        // Simulate a Catalog failure after the SDK commits a source-wide change.
        // Writable reopen uses the persisted object revision to finish the projection.
        const auto updatedReceipt=timeDepthDependency(f.session.relationRequest(mapA,&f.error));
        QVERIFY(!updatedReceipt.isEmpty());
        const auto nextA=publish(rawA,updatedReceipt);
        QVERIFY(!nextA.isEmpty());
        f.catalog.debugAbortJournalAfter(1);
        QVERIFY(paleo::saveTimeDepthAlignment(&f.catalog,{{"sources",QVariantList{a.id}},
            {"unit","ms"},{"type","TWT"},{"reference","new-datum"}},&f.error));
        QVERIFY(!f.error.isEmpty());
        QVERIFY(!stale(nextA));
        QVERIFY(!stale(productTie));
        f.catalog.debugAbortJournalAfter(0);
        f.session.close();
        QVERIFY(f.catalog.open(f.project,&f.error));
        QVERIFY(f.bind(true));
        QVERIFY(!stale(nextA));
        f.session.close();
        QVERIFY(f.bind());
        QVERIFY(stale(nextA));
        QVERIFY(stale(productTie));
        QVERIFY(!stale(productB));
        QVERIFY(!stale(rawA));
        const auto revision=f.catalog.catalogRevision();
        QVERIFY(refreshTimeDepthDependencies(f.catalog,&f.error));
        QCOMPARE(f.catalog.catalogRevision(),revision);
        auto remove=f.session.command(SourceCommand::Kind::RemoveBinding);
        remove.bindingId="map-b";
        QVERIFY(runCommand(f,remove).ok);
        QVERIFY(stale(productB));
    }
};
QTEST_GUILESS_MAIN(TestDocumentSources)
#include "tst_document_sources.moc"
