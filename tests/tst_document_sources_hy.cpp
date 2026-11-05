#include <QtTest>
#include "fixtures/documentsourcefixture.h"
#include "io/sourcefingerprint.h"
#include "services/documentsourceprobe.h"
#include "services/documentrelationresolver.h"
#include <paleo_data/object_descriptor.h>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QRegularExpression>

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
    void relocationUsesOnlyAPrivateCopyOfRealLogData() {
        auto logs=files(QStringLiteral("岩心"),{"*.las"});QVERIFY(!logs.isEmpty());
        logs.removeIf([](const auto &path){return QFileInfo(path).size()==0;});QVERIFY(!logs.isEmpty());
        std::sort(logs.begin(),logs.end(),[](const auto &a,const auto &b){return QFileInfo(a).size()<QFileInfo(b).size();});
        QFile original(logs.front());QVERIFY(original.open(QIODevice::ReadOnly));const auto bytes=original.readAll();original.close();
        const auto digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
        Fixture f;QVERIFY(f.init());const auto copy=f.temp.filePath("private.las");QVERIFY(write(copy,bytes));QVERIFY2(f.importModel(copy),qPrintable(f.error));QVERIFY(f.bind());
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
