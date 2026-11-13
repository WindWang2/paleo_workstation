// 层：测试壳
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>
#include <cstdlib>
#include "metadata/projectlock.h"
#include "catalog/purgelease.h"
#include "workflow/capabilitygateway.h"

using paleo::capability::OperationLog;
namespace {
OperationLog::Record seed() {
  OperationLog::Record record;
  record.documentId = "doc"; record.callerScope = "agent:session-one"; record.clientRequestId = "same";
  record.capabilityId = "constraint.idw"; record.implementationVersion = "1";
  record.requestHash = QString(64, 'a'); record.planHash = QString(64, 'b');
  record.frozenRequest = {{"bindingMode", "pinned-value"}, {"intent", "compute-only"}};
  return record;
}
QString logDir(const QString &project) { return QDir(project).filePath(".paleo/capability-ops"); }
QString update(OperationLog &log, OperationLog::Record &record,
               OperationLog::Transition transition = OperationLog::Transition::Advance) {
  OperationLog::Record current;
  const auto result = log.saveDurable(record, record.stateRevision, record.ownerAttempt, &current, transition);
  if (result == QLatin1String("ok")) record = current;
  return result;
}
QString begin(OperationLog &log, OperationLog::Record &record) {
  const auto oldAttempt = record.ownerAttempt;
  record.ownerAttempt = "attempt-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  record.ownerProcess = QString::number(QCoreApplication::applicationPid());
  record.execution = "running"; record.code = "running"; ++record.computeCount;
  OperationLog::Record current;
  const auto result = log.saveDurable(record, record.stateRevision, oldAttempt, &current);
  if (result == QLatin1String("ok")) record = current;
  return result;
}

int child(const QString &root, const QString &point) {
  ProjectDirLock lock(root); QString error;
  if (!lock.tryLock(&error)) return 73;
  OperationLog log(logDir(root), root, lock.claim());
  const auto accepted = log.acceptDurable(seed());
  if (accepted.code != QLatin1String("created")) return 74;
  auto record = accepted.record;
  if (point == QLatin1String("queued")) std::_Exit(91);
  if (begin(log, record) != QLatin1String("ok")) return 75;
  if (point == QLatin1String("running")) std::_Exit(92);
  if (point == QLatin1String("partial")) {
    QFile partial(QDir(logDir(root)).filePath("partial.bin"));
    if (!partial.open(QIODevice::WriteOnly) || partial.write("partial") != 7 || !partial.flush()) return 76;
    std::_Exit(93);
  }
  // Control-plane fault fixture. Real receipt/member transaction crashes are
  // exercised separately through the publication/recovery coordinator.
  record.execution = "prepared"; record.preparedManifest = {{"kind", "control-fixture"}};
  if (update(log, record) != QLatin1String("ok")) return 77;
  record.commitState = "publishing";
  if (update(log, record) != QLatin1String("ok")) return 78;
  std::_Exit(94);
}
}

class TestOperationControl : public QObject {
  Q_OBJECT
private slots:
  void writesRequireAnActualProjectClaim();
  void stableIdentityAndReadOnlyLookup();
  void pathAliasesShareTheDurableIdentity();
  void compareAndSwapFencesOldCallbacks();
  void preparedAndCommittedFactsCannotBeReopened();
  void persistFailureKeepsThePreviousRecord();
  void eventsStayBoundedWithoutResettingRevision();
  void realProcessCrashLeavesOneOperation_data();
  void realProcessCrashLeavesOneOperation();
  void aSecondProcessCannotAcceptWhileOwned();
  void purgeLeaseStopsNewRootsButPreservesExistingFacts();
};

void TestOperationControl::purgeLeaseStopsNewRootsButPreservesExistingFacts() {
  QTemporaryDir root;ProjectDirLock owner(root.path());QVERIFY(owner.tryLock());
  OperationLog log(logDir(root.path()),root.path(),owner.claim());const auto first=log.acceptDurable(seed());
  QCOMPARE(first.code,QString("created"));
  auto lease=CatalogPurgeLease::acquire(QDir(root.path()).filePath("artifacts/metadata/catalog.json"));QVERIFY(lease);
  auto next=seed();next.clientRequestId="next";
  QCOMPARE(log.acceptDurable(next).code,QString("catalog_purge_in_progress"));
  QCOMPARE(log.lookup("doc","agent:session-one","same").code,QString("found"));
  auto existing=first.record;existing.cancelRequested=true;existing.code="cancel_requested";
  QCOMPARE(update(log,existing),QString("ok"));
  lease.reset();QCOMPARE(log.acceptDurable(next).code,QString("created"));
}

void TestOperationControl::writesRequireAnActualProjectClaim() {
  QTemporaryDir root;
  OperationLog unowned(logDir(root.path()));
  QCOMPARE(unowned.acceptDurable(seed()).code, QStringLiteral("project_write_ownership_required"));
  QVERIFY(!QFile::exists(QDir(logDir(root.path())).filePath("operations.json")));
  ProjectDirLock owner(root.path()); QVERIFY(owner.tryLock());
  OperationLog outside(root.filePath("elsewhere"), root.path(), owner.claim());
  QCOMPARE(outside.acceptDurable(seed()).code, QStringLiteral("project_write_ownership_required"));
}

void TestOperationControl::stableIdentityAndReadOnlyLookup() {
  QTemporaryDir root;
  ProjectDirLock owner(root.path()); QVERIFY(owner.tryLock());
  OperationLog log(logDir(root.path()), root.path(), owner.claim());
  const auto first = log.acceptDurable(seed()); QCOMPARE(first.code, QStringLiteral("created"));
  QCOMPARE(log.acceptDurable(seed()).record.operationId, first.record.operationId);
  auto changed = seed(); changed.requestHash = QString(64, 'c');
  QCOMPARE(log.acceptDurable(changed).code, QStringLiteral("conflict"));
  OperationLog reader(logDir(root.path()));
  QCOMPARE(reader.lookup("doc", "agent:session-one", "same").record.operationId, first.record.operationId);
  QCOMPARE(reader.lookup("doc", "agent:session-two", "same").code, QStringLiteral("not_found"));
  QCOMPARE(reader.acceptDurable(seed()).code, QStringLiteral("project_write_ownership_required"));
}

void TestOperationControl::pathAliasesShareTheDurableIdentity() {
  QTemporaryDir root;
  ProjectDirLock owner(root.path()); QVERIFY(owner.tryLock());
  OperationLog first(logDir(root.path()), root.path(), owner.claim());
  const auto alias = root.path() + QStringLiteral("/./");
  OperationLog second(logDir(alias), alias, owner.claim());
  const auto accepted = first.acceptDurable(seed()); QCOMPARE(accepted.code, QStringLiteral("created"));
  const auto replay = second.acceptDurable(seed()); QCOMPARE(replay.code, QStringLiteral("replay"));
  QCOMPARE(replay.record.operationId, accepted.record.operationId);
}

void TestOperationControl::compareAndSwapFencesOldCallbacks() {
  QTemporaryDir root;
  ProjectDirLock owner(root.path()); QVERIFY(owner.tryLock());
  OperationLog log(logDir(root.path()), root.path(), owner.claim());
  auto record = log.acceptDurable(seed()).record; QCOMPARE(begin(log, record), QStringLiteral("ok"));
  const auto old = record;
  record.cancelRequested = true; QCOMPARE(update(log, record), QStringLiteral("ok"));
  auto callback = old; callback.execution = "prepared"; callback.preparedManifest = {{"ready", true}};
  QCOMPARE(log.saveDurable(callback, old.stateRevision, old.ownerAttempt), QStringLiteral("operation_state_conflict"));
  QCOMPARE(log.saveDurable(record, record.stateRevision, "different-attempt"), QStringLiteral("operation_state_conflict"));
  record.cancelRequested = false; QCOMPARE(update(log, record), QStringLiteral("invalid_operation_transition"));
}

void TestOperationControl::preparedAndCommittedFactsCannotBeReopened() {
  QTemporaryDir root;
  ProjectDirLock owner(root.path()); QVERIFY(owner.tryLock());
  OperationLog log(logDir(root.path()), root.path(), owner.claim());
  auto record = log.acceptDurable(seed()).record; QCOMPARE(begin(log, record), QStringLiteral("ok"));
  record.execution = "prepared"; record.preparedManifest = {{"manifestHash", "immutable"}};
  QCOMPARE(update(log, record), QStringLiteral("ok"));
  auto tampered = record; tampered.preparedManifest = {{"manifestHash", "changed"}};
  QCOMPARE(update(log, tampered), QStringLiteral("invalid_operation_transition"));
  record.commitState = "publishing"; QCOMPARE(update(log, record), QStringLiteral("ok"));
  auto forbidden = record; forbidden.commitState = "not_requested";
  QCOMPARE(update(log, forbidden), QStringLiteral("invalid_operation_transition"));
  record.commitState = "committed"; record.execution = "succeeded";
  QCOMPARE(update(log, record), QStringLiteral("ok"));
  auto restart = record; restart.execution = "running"; ++restart.computeCount;
  QCOMPARE(update(log, restart, OperationLog::Transition::Resume), QStringLiteral("invalid_operation_transition"));
  record.presentation = "pending"; QCOMPARE(update(log, record), QStringLiteral("ok"));
  record.presentation = "superseded"; QCOMPARE(update(log, record), QStringLiteral("ok"));
  QCOMPARE(record.computeCount, 1);
}

void TestOperationControl::persistFailureKeepsThePreviousRecord() {
  QTemporaryDir root;
  ProjectDirLock owner(root.path()); QVERIFY(owner.tryLock());
  OperationLog log(logDir(root.path()), root.path(), owner.claim());
  auto record = log.acceptDurable(seed()).record;
  const auto previous = record;
  OperationLog::armOperationFileWriteFault(0);
  QCOMPARE(begin(log, record), QStringLiteral("persist_failed"));
  OperationLog::armOperationFileWriteFault(-1);
  QCOMPARE(log.recordById(previous.operationId), previous);
}

void TestOperationControl::eventsStayBoundedWithoutResettingRevision() {
  QTemporaryDir root;
  ProjectDirLock owner(root.path()); QVERIFY(owner.tryLock());
  OperationLog log(logDir(root.path()), root.path(), owner.claim());
  auto record = log.acceptDurable(seed()).record; QCOMPARE(begin(log, record), QStringLiteral("ok"));
  for (int i = 0; i < 140; ++i) { record.code = QString::number(i); QCOMPARE(update(log, record), QStringLiteral("ok")); }
  QCOMPARE(record.events.size(), 128); QCOMPARE(record.stateRevision, quint64(142));
  QCOMPARE(record.events.last().toObject().value("revision"), QJsonValue("142"));
  QVERIFY(record.events.first().toObject().value("revision").toString().toULongLong() > 1);
}

void TestOperationControl::realProcessCrashLeavesOneOperation_data() {
  QTest::addColumn<QString>("point"); QTest::addColumn<int>("exitCode");
  QTest::newRow("accepted") << QStringLiteral("queued") << 91;
  QTest::newRow("worker-running") << QStringLiteral("running") << 92;
  QTest::newRow("partial-stage") << QStringLiteral("partial") << 93;
  QTest::newRow("publishing") << QStringLiteral("publishing") << 94;
}

void TestOperationControl::realProcessCrashLeavesOneOperation() {
  QFETCH(QString, point); QFETCH(int, exitCode);
  QTemporaryDir root;
  QProcess process; process.start(QCoreApplication::applicationFilePath(), {"--control-child", root.path(), point});
  QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), exitCode);
  ProjectDirLock owner(root.path()); QVERIFY2(owner.tryLock(), qPrintable(process.readAllStandardError()));
  OperationLog log(logDir(root.path()), root.path(), owner.claim());
  const auto old = log.lookup("doc", "agent:session-one", "same"); QCOMPARE(old.code, QStringLiteral("found"));
  const auto retry = log.acceptDurable(seed()); QCOMPARE(retry.code, QStringLiteral("replay"));
  QCOMPARE(retry.record.operationId, old.record.operationId); QCOMPARE(retry.record.computeCount, old.record.computeCount);
  QCOMPARE(retry.record.commitState, old.record.commitState);
}

void TestOperationControl::aSecondProcessCannotAcceptWhileOwned() {
  QTemporaryDir root;
  ProjectDirLock owner(root.path()); QVERIFY(owner.tryLock());
  QProcess process; process.start(QCoreApplication::applicationFilePath(), {"--control-child", root.path(), "queued"});
  QVERIFY(process.waitForFinished(10000)); QCOMPARE(process.exitCode(), 73);
  QVERIFY(!QFile::exists(QDir(logDir(root.path())).filePath("operations.json")));
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().value(1) == QLatin1String("--control-child"))
    return child(app.arguments().value(2), app.arguments().value(3));
  TestOperationControl test; return QTest::qExec(&test, argc, argv);
}
#include "tst_operation_control.moc"
