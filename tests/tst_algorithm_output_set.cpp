#include <QtTest>
#include <algorithm>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include "../src/catalog/datacatalog.h"
#include "../src/catalog/catalogstore.h"
#include "../src/workflow/algorithmpublish.h"

namespace {
bool write(const QString &path,const QByteArray &bytes) {
  QFile file(path);if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate))return false;
  return file.write(bytes)==bytes.size();
}
struct Fixture {
  QTemporaryDir directory;
  DataCatalog catalog;
  DerivedAssetRegistrar registrar;
  AlgorithmSpec spec;
  AlgorithmRunResult result;
  QVector<paleo::AlgorithmOutputTarget> targets;
  bool init() {
    if(!directory.isValid() || !catalog.open(directory.path()))return false;
    registrar=DerivedAssetRegistrar(&catalog,directory.path());
    spec.id="test:estimate_and_quality";spec.complete=true;spec.engine="native_cpp";
    for(const QString &port : {QString("OUTPUT"),QString("QUALITY")}) {
      AlgoPort output;output.id=port;output.kind="raster";output.required=true;spec.outputs.append(output);
      const auto staging=registrar.stage("raster",port,port+".tif");
      if(!staging.isValid() || !write(staging.absolutePath,port.toUtf8()))return false;
      targets.append({port,staging});
      AlgorithmOutputArtifact artifact;artifact.portId=port;artifact.absolutePath=staging.absolutePath;
      result.artifacts.append(artifact);
    }
    result.ok=true;return true;
  }
  bool prepare(DerivedPreparedSet &prepared,QString &error) {
    return paleo::prepareAlgorithmOutputSet(spec,result,targets,prepared,&error);
  }
};
}
class AlgorithmOutputSetTests : public QObject {
  Q_OBJECT
private slots:
  void requiredOutputsCommitTogetherAndSurviveReopen();
  void siblingDependenciesAreAtomicAndAcyclic();
  void missingRequiredAndDuplicatePortsAreRejected();
  void optionalOutputCanBeAbsent();
  void publicationRechecksRequiredPorts();
  void changedFileAndParentRejectWholeSet();
  void failedSqlFlushRollsBackAndCanRetry();
  void enclosingBatchCannotProduceReadyReceipt();
  void stagingCatalogCannotProducePersistentReceipt();
  void preparationBudgetAndCancellationPreserveExternalFiles();
  void processingAdapterRetainsDeclaredOutputsAndLegacyIsSingle();
  void operationReceiptReplaysWithoutAnotherVersion();
  void operationReceiptRollsBackWithItsMembers();
};

void AlgorithmOutputSetTests::operationReceiptReplaysWithoutAnotherVersion() {
  Fixture f; QVERIFY(f.init()); DerivedPreparedSet prepared; QString error;
  QVERIFY2(f.prepare(prepared, error), qPrintable(error));
  prepared.publicationSchema = 2; prepared.id = "set-operation-one"; prepared.documentId = "doc";
  prepared.operationId = "op-one"; prepared.requestHash = QString(64, 'a'); prepared.planHash = QString(64, 'b');
  prepared.storeGeneration = f.catalog.publication("doc", "op-one", prepared.requestHash).storeGeneration;
  DerivedOutputSetReceipt receipt;
  QVERIFY2(f.registrar.commitSet(f.spec, prepared, {}, f.spec.id, {}, receipt), qPrintable(receipt.error));
  QCOMPARE(receipt.toVariantMap().value("schema").toInt(), 2);
  QCOMPARE(receipt.toVariantMap().value("outputSetId").toString(), prepared.id);
  QCOMPARE(f.catalog.publication("doc", "op-one", prepared.requestHash).code, QStringLiteral("committed"));
  const auto original = receipt.toVariantMap();
  DerivedAssetRegistrar replay(&f.catalog, f.directory.path());
  QVERIFY2(replay.commitSet(f.spec, prepared, {}, f.spec.id, {}, receipt), qPrintable(receipt.error));
  QCOMPARE(receipt.toVariantMap(), original); QCOMPARE(f.catalog.versions().size(), 2);
  auto changed = prepared; changed.members[0].fingerprint.sha256 = QString(64, 'c');
  QVERIFY(!replay.commitSet(f.spec, changed, {}, f.spec.id, {}, receipt));
  QCOMPARE(receipt.code, QStringLiteral("publication_conflict")); QCOMPARE(f.catalog.versions().size(), 2);
  QVERIFY(!f.catalog.updateVersionExtra(prepared.members[0].staging.versionId, "outputSet", QVariantMap{}, &error));
  DataCatalog reopened; QVERIFY(reopened.open(f.directory.path()));
  QCOMPARE(reopened.publication("doc", "op-one", prepared.requestHash).code, QStringLiteral("committed"));
}

void AlgorithmOutputSetTests::operationReceiptRollsBackWithItsMembers() {
  Fixture f; QVERIFY(f.init()); DerivedPreparedSet prepared; QString error;
  QVERIFY(f.prepare(prepared, error));
  prepared.publicationSchema = 2; prepared.id = "set-operation-rollback"; prepared.documentId = "doc";
  prepared.operationId = "op-rollback"; prepared.requestHash = QString(64, 'a'); prepared.planHash = QString(64, 'b');
  prepared.storeGeneration = f.catalog.publication("doc", prepared.operationId, prepared.requestHash).storeGeneration;
  const auto connection = QStringLiteral("publication-trigger");
  {
    auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
    db.setDatabaseName(CatalogStore::sqlitePathFor(f.directory.path())); QVERIFY(db.open());
    QSqlQuery query(db);
    QVERIFY(query.exec("CREATE TRIGGER reject_publication BEFORE INSERT ON paleo_operation_publications "
                       "BEGIN SELECT RAISE(ABORT,'injected publication failure'); END"));
    db.close();
  }
  QSqlDatabase::removeDatabase(connection);
  DerivedOutputSetReceipt receipt;
  QVERIFY(!f.registrar.commitSet(f.spec, prepared, {}, f.spec.id, {}, receipt));
  QVERIFY(!receipt.ready); QVERIFY(f.catalog.versions().isEmpty()); QVERIFY(f.catalog.assets().isEmpty());
  QCOMPARE(f.catalog.publication("doc", prepared.operationId, prepared.requestHash).code,
           QStringLiteral("not_committed"));
}
void AlgorithmOutputSetTests::requiredOutputsCommitTogetherAndSurviveReopen() {
  Fixture f;QVERIFY(f.init());DerivedPreparedSet prepared;QString error;QVERIFY2(f.prepare(prepared,error),qPrintable(error));
  QCOMPARE(f.catalog.assets().size(),0);
  DerivedOutputSetReceipt receipt;
  QVERIFY2(f.registrar.commitSet(f.spec,prepared,{},f.spec.id,{},receipt),qPrintable(receipt.error));
  QVERIFY(receipt.ready);QCOMPARE(receipt.members.size(),2);QCOMPARE(receipt.code,QString("committed"));
  QCOMPARE(f.catalog.assets().size(),2);QCOMPARE(f.catalog.versions().size(),2);
  DataCatalog reopened;QVERIFY(reopened.open(f.directory.path()));
  for(const auto &member : receipt.members) {
    const auto version=reopened.versionById(member.versionId);
    QCOMPARE(version.assetId,member.assetId);QCOMPARE(version.sha256,member.sha256);
    QCOMPARE(version.extra.value("outputSet").toMap(),receipt.toVariantMap());
    QCOMPARE(DataCatalog::sha256FileHex(reopened.versionFilePath(version)),member.sha256);
  }
}
void AlgorithmOutputSetTests::missingRequiredAndDuplicatePortsAreRejected() {
  Fixture f;QVERIFY(f.init());auto saved=f.result;f.result.artifacts.removeLast();f.targets.removeLast();
  DerivedPreparedSet prepared;QString error;QVERIFY(!f.prepare(prepared,error));QVERIFY(prepared.members.isEmpty());QVERIFY(error.contains("QUALITY"));
  f.result=saved;f.result.artifacts[1].portId="OUTPUT";f.targets.append({"OUTPUT",f.targets.first().staging});
  QVERIFY(!f.prepare(prepared,error));QVERIFY(f.catalog.assets().isEmpty());QVERIFY(f.catalog.versions().isEmpty());
}
void AlgorithmOutputSetTests::siblingDependenciesAreAtomicAndAcyclic() {
  Fixture f;QVERIFY(f.init());f.result.artifacts[1].parentPortIds={"OUTPUT"};
  DerivedPreparedSet prepared;QString error;QVERIFY(f.prepare(prepared,error));
  std::reverse(prepared.members.begin(),prepared.members.end());
  DerivedOutputSetReceipt receipt;QVERIFY2(f.registrar.commitSet(f.spec,prepared,{},f.spec.id,{},receipt),qPrintable(receipt.error));
  QCOMPARE(f.catalog.versionById(f.targets[1].staging.versionId).parentVersionIds,QStringList{f.targets[0].staging.versionId});
  Fixture cyclic;QVERIFY(cyclic.init());cyclic.result.artifacts[0].parentPortIds={"QUALITY"};cyclic.result.artifacts[1].parentPortIds={"OUTPUT"};
  QVERIFY(cyclic.prepare(prepared,error));QVERIFY(!cyclic.registrar.commitSet(cyclic.spec,prepared,{},cyclic.spec.id,{},receipt));
  QCOMPARE(receipt.code,QStringLiteral("invalid_output_dependencies"));QVERIFY(cyclic.catalog.versions().isEmpty());
}
void AlgorithmOutputSetTests::optionalOutputCanBeAbsent() {
  Fixture f;QVERIFY(f.init());f.spec.outputs[1].required=false;f.result.artifacts.removeLast();f.targets.removeLast();
  DerivedPreparedSet prepared;QString error;QVERIFY2(f.prepare(prepared,error),qPrintable(error));DerivedOutputSetReceipt receipt;
  QVERIFY2(f.registrar.commitSet(f.spec,prepared,{},f.spec.id,{},receipt),qPrintable(receipt.error));QCOMPARE(receipt.members.size(),1);
}
void AlgorithmOutputSetTests::publicationRechecksRequiredPorts() {
  Fixture f;QVERIFY(f.init());DerivedPreparedSet prepared;QString error;QVERIFY(f.prepare(prepared,error));
  prepared.members.removeLast();DerivedOutputSetReceipt receipt;
  QVERIFY(!f.registrar.commitSet(f.spec,prepared,{},f.spec.id,{},receipt));QCOMPARE(receipt.code,QString("required_output_missing"));
  QVERIFY(!receipt.ready);QVERIFY(receipt.members.isEmpty());QVERIFY(f.catalog.assets().isEmpty());QVERIFY(f.catalog.versions().isEmpty());
}
void AlgorithmOutputSetTests::changedFileAndParentRejectWholeSet() {
  Fixture f;QVERIFY(f.init());DerivedPreparedSet prepared;QString error;QVERIFY(f.prepare(prepared,error));DerivedOutputSetReceipt receipt;
  QVERIFY(!f.registrar.commitSet(f.spec,prepared,{"missing-input"},f.spec.id,{},receipt));QCOMPARE(receipt.code,QString("input_version_missing"));
  QVERIFY(write(prepared.members.last().staging.absolutePath,"changed bytes"));
  QVERIFY(!f.registrar.commitSet(f.spec,prepared,{},f.spec.id,{},receipt));QCOMPARE(receipt.code,QString("output_changed"));
  QVERIFY(!receipt.ready);QVERIFY(receipt.members.isEmpty());QVERIFY(f.catalog.assets().isEmpty());QVERIFY(f.catalog.versions().isEmpty());
}
void AlgorithmOutputSetTests::failedSqlFlushRollsBackAndCanRetry() {
  Fixture f;QVERIFY(f.init());DerivedPreparedSet prepared;QString error;QVERIFY(f.prepare(prepared,error));
  const auto connection=QString("output-set-failure");
  {
    auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(f.catalog.sqliteCatalogPath());QVERIFY(db.open());
    QSqlQuery sql(db);QVERIFY(sql.exec(QString("CREATE TRIGGER reject_output_set BEFORE INSERT ON versions WHEN NEW.id='%1' BEGIN SELECT RAISE(ABORT,'output-set fault'); END").arg(prepared.members.last().staging.versionId)));
    DerivedOutputSetReceipt receipt;
    QVERIFY(!f.registrar.commitSet(f.spec,prepared,{},f.spec.id,{},receipt));QCOMPARE(receipt.code,QString("catalog_commit_failed"));
    QVERIFY(!receipt.ready);QVERIFY(receipt.members.isEmpty());QVERIFY(f.catalog.assets().isEmpty());QVERIFY(f.catalog.versions().isEmpty());
    {DataCatalog reopened;QVERIFY(reopened.open(f.directory.path()));QVERIFY(reopened.assets().isEmpty());QVERIFY(reopened.versions().isEmpty());}
    QVERIFY(sql.exec("DROP TRIGGER reject_output_set"));
    sql.finish();db.close();
    QVERIFY2(f.registrar.commitSet(f.spec,prepared,{},f.spec.id,{},receipt),qPrintable(receipt.error));QVERIFY(receipt.ready);QCOMPARE(f.catalog.versions().size(),2);
    db.close();
  }
  QSqlDatabase::removeDatabase(connection);
}
void AlgorithmOutputSetTests::enclosingBatchCannotProduceReadyReceipt() {
  Fixture f;QVERIFY(f.init());DerivedPreparedSet prepared;QString error;QVERIFY(f.prepare(prepared,error));
  DataCatalog::BatchSave outer(&f.catalog);DerivedOutputSetReceipt receipt;
  QVERIFY(!f.registrar.commitSet(f.spec,prepared,{},f.spec.id,{},receipt));QCOMPARE(receipt.code,QString("batch_in_progress"));
  QVERIFY(!receipt.ready);QVERIFY(f.catalog.versions().isEmpty());outer.abort();
}
void AlgorithmOutputSetTests::stagingCatalogCannotProducePersistentReceipt() {
  Fixture f;QVERIFY(f.init());DerivedPreparedSet prepared;QString error;QVERIFY(f.prepare(prepared,error));
  auto snapshot=f.catalog.createStagingCopy({});QVERIFY(snapshot);
  DerivedAssetRegistrar trial(snapshot.get(),f.directory.path());DerivedOutputSetReceipt receipt;
  QVERIFY(!trial.commitSet(f.spec,prepared,{},f.spec.id,{},receipt));QCOMPARE(receipt.code,QString("staging_catalog"));
  QVERIFY(!receipt.ready);QVERIFY(f.catalog.versions().isEmpty());QVERIFY(snapshot->versions().isEmpty());
}
void AlgorithmOutputSetTests::preparationBudgetAndCancellationPreserveExternalFiles() {
  Fixture f;QVERIFY(f.init());
  QStringList originalHashes;
  for(auto &artifact : f.result.artifacts) {
    const auto source=f.directory.filePath(artifact.portId+"-external.bin");QVERIFY(write(source,QByteArray(2*1024*1024,'x')));
    QVERIFY(QFile::remove(artifact.absolutePath));artifact.absolutePath=source;
    originalHashes.append(DataCatalog::sha256FileHex(source));
  }
  DerivedPreparedSet prepared;QString error;paleo::OutputSetPreparationBudget budget;budget.maxTotalBytes=1024;
  QVERIFY(!paleo::prepareAlgorithmOutputSet(f.spec,f.result,f.targets,prepared,&error,{},budget));
  int checks=0;QVERIFY(!paleo::prepareAlgorithmOutputSet(f.spec,f.result,f.targets,prepared,&error,[&]{return ++checks>3;}));
  QVERIFY(prepared.members.isEmpty());
  for(qsizetype i=0;i<f.result.artifacts.size();++i) {
    QCOMPARE(QFileInfo(f.result.artifacts[i].absolutePath).size(),qint64(2*1024*1024));QVERIFY(!QFileInfo::exists(f.targets[i].staging.absolutePath));
    QCOMPARE(DataCatalog::sha256FileHex(f.result.artifacts[i].absolutePath),originalHashes[i]);
  }
  QVERIFY(paleo::prepareAlgorithmOutputSet(f.spec,f.result,f.targets,prepared,&error));
  QCOMPARE(prepared.members.size(),2);QVERIFY(f.catalog.assets().isEmpty());
}
void AlgorithmOutputSetTests::processingAdapterRetainsDeclaredOutputsAndLegacyIsSingle() {
  Fixture f;QVERIFY(f.init());
  const QVariantMap outputs{{"OUTPUT",f.result.artifacts[0].absolutePath},{"QUALITY",f.result.artifacts[1].absolutePath}};
  const auto all=paleo::runResultFromProcessingOutputs(f.spec,outputs);QVERIFY(all.ok);QCOMPARE(all.artifacts.size(),2);
  QVERIFY(!paleo::runResultFromProcessingOutputs(f.spec,{{"OUTPUT",outputs.value("OUTPUT")}}).ok);
  const auto legacy=paleo::runResultFromProcessingOutput(outputs);QVERIFY(legacy.ok);QCOMPARE(legacy.artifacts.size(),1);
  QString error;QVERIFY(paleo::publishArtifacts(f.registrar,f.targets[0].staging,all,{},f.spec.id,{},&error));
  QCOMPARE(f.catalog.versions().size(),1);QVERIFY(f.catalog.versionById(f.targets[1].staging.versionId).id.isEmpty());
}
QTEST_GUILESS_MAIN(AlgorithmOutputSetTests)
#include "tst_algorithm_output_set.moc"
