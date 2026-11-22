// 层：测试壳
// G4-W2/W3/W4/W5/W7/W8：真实持久文件、跨实例重启与磁盘篡改下的记录完整性、
// 私有目录边界与错误可区分性。全部经生产入口（acceptDurable/saveDurable/
// start/attachExternalResults/get/results/read/operationRetention）验证，
// 不单测正则、不 mock 返回值；目录副作用用真实文件树快照对比。
#include <QtTest>
#include "workflow/capabilityrecovery.h"
#include "workflow/capabilitypublication_internal.h"
#include "workflow/operationprivatedir.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include "services/paleotaskservice.h"
#include "services/operationretention.h"
#include <QCryptographicHash>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#ifdef Q_OS_WIN
#include <QProcess>
#endif
#include <QUuid>

using namespace paleo;
using namespace paleo::capability;

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString logDir(const QString &root) { return QDir(root).filePath(".paleo/capability-ops"); }
QString journalPath(const QString &root) { return QDir(logDir(root)).filePath("operations.json"); }

bool linkDirectory(const QString &target, const QString &alias) {
#ifdef Q_OS_WIN
  QProcess junction;
  junction.setProgram(QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/cmd.exe"));
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("PALEO_TEST_JUNCTION_LINK", QDir::toNativeSeparators(alias));
  environment.insert("PALEO_TEST_JUNCTION_TARGET", QDir::toNativeSeparators(target));
  junction.setProcessEnvironment(environment);
  junction.setNativeArguments(QStringLiteral(
      "/d /v:off /s /c \"mklink /J \"%PALEO_TEST_JUNCTION_LINK%\" \"%PALEO_TEST_JUNCTION_TARGET%\"\""));
  junction.start();
  return junction.waitForFinished() && junction.exitStatus() == QProcess::NormalExit &&
      junction.exitCode() == 0 && QFileInfo(alias).isJunction();
#else
  return QFile::link(target, alias);
#endif
}

OperationLog::Record seed(const QString &request = "req") {
  OperationLog::Record record;
  record.documentId = "doc"; record.callerScope = "agent:g4"; record.clientRequestId = request;
  record.capabilityId = "g4.compute"; record.implementationVersion = "1";
  record.requestHash = QString(64, 'a'); record.planHash = QString(64, 'b');
  record.frozenRequest = {{"intent", "g4"}};
  return record;
}
QString begin(OperationLog &log, OperationLog::Record &record, const QString &prefix = QString()) {
  const auto oldAttempt = record.ownerAttempt;
  record.ownerAttempt = prefix + uuid();
  record.ownerProcess = QString::number(QCoreApplication::applicationPid());
  record.execution = "running"; record.code = "running"; ++record.computeCount;
  OperationLog::Record current;
  const auto result = log.saveDurable(record, record.stateRevision, oldAttempt, &current);
  if (result == QLatin1String("ok")) record = current;
  return result;
}
QString update(OperationLog &log, OperationLog::Record &record) {
  OperationLog::Record current;
  const auto result = log.saveDurable(record, record.stateRevision, record.ownerAttempt, &current);
  if (result == QLatin1String("ok")) record = current;
  return result;
}

OperationWorker countingWorker(std::shared_ptr<int> calls) {
  return [calls](const OperationWorkContext &c) {
    ++*calls;
    OperationWorkResult r;
    QFile file(c.privateDir + "/values.bin");
    r.ok = file.open(QIODevice::WriteOnly) && file.write("real-worker-bytes") == 17;
    file.close();
    r.outputs.ok = r.ok;
    r.outputs.artifacts.append({"OUTPUT", file.fileName(), {}, {}, {}, {}});
    return r;
  };
}
FrozenOperation plan(const QString &request, const OperationWorker &worker) {
  FrozenOperation p;
  p.capabilityId = "g4.compute"; p.implementationVersion = "1";
  p.callerScope = "agent:g4"; p.clientRequestId = request;
  p.intent = {{"number", 1}}; p.frozenPlan = {{"immutable", true}}; p.phase = "publish";
  p.worker = worker;
  p.outputSpec.id = "g4:values"; p.outputSpec.complete = true;
  AlgoPort port; port.id = "OUTPUT"; port.kind = "file";
  p.outputSpec.outputs = {port}; p.outputPorts = {"OUTPUT"};
  return p;
}

struct Host {
  DataCatalog catalog;
  std::unique_ptr<ProjectDirLock> lock;
  PaleoTaskService tasks;
  CapabilityRecovery recovery;
  bool allowExternal = false;
  QString documentId = "doc";
  explicit Host(const QString &root) {
    lock = std::make_unique<ProjectDirLock>(root);
    QVERIFY2(lock->tryLock(), "project lock");
    QVERIFY2(catalog.open(root), "catalog open");
    recovery.setTaskService(&tasks);
    recovery.setHostProvider([this, root] {
      RecoveryHost h;
      h.projectDir = root; h.operationDir = logDir(root); h.catalog = &catalog;
      h.writeClaim = lock->claim();
      h.context.documentId = documentId; h.context.epoch = 1; h.context.open = true;
      h.context.persistentIdentity = true; h.context.readOnly = false; h.context.ownsWrites = true;
      h.context.allowedEffects = host::Read | host::PrivateStage | host::CatalogPublish | host::Presentation;
      if (allowExternal) h.context.allowedEffects |= host::ExternalTransfer;
      return h;
    });
  }
};

// 磁盘篡改：按生产写入口同样的 Compact JSON 重写 journal，模拟攻击者或
// 位翻转后的持久状态。written 接收最终落盘字节，供"无损"断言对照；
// 失败返回 false（QVERIFY 宏不能出现在带返回值的 helper 里）。
bool rewriteJournal(const QString &root, const std::function<void(QJsonArray &)> &edit, QByteArray &written) {
  const QString path = journalPath(root);
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly)) return false;
  const QByteArray before = input.readAll();
  input.close();
  const auto document = QJsonDocument::fromJson(before);
  if (!document.isArray()) return false;
  auto rows = document.array();
  edit(rows);
  written = QJsonDocument(rows).toJson(QJsonDocument::Compact);
  QFile output(path);
  if (!output.open(QIODevice::WriteOnly)) return false;
  output.write(written);
  output.close();
  return true;
}
QByteArray readFileBytes(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll();
}
QStringList treeSnapshot(const QString &root) {
  QStringList entries;
  QDirIterator it(root, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) entries.append(QDir(root).relativeFilePath(it.next()));
  entries.sort();
  return entries;
}
bool runToEnd(Host &host, const OperationLog::Record &record, ExecuteResult &result, QString *error) {
  bool done = false;
  error->clear();
  auto *task = host.recovery.start(record.callerScope, record.operationId, &host.recovery,
      "g4", [&](ExecuteResult r) { result = r; done = true; }, false, error);
  if (!task) return false;
  QEventLoop loop;
  QObject::connect(task, &PaleoTask::finished, &loop, &QEventLoop::quit);
  loop.exec();
  // finished 回调与本 loop 同线程直连且先注册先执行；有界自旋兜底。
  for (int i = 0; i < 200 && !done; ++i) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  return done;
}
} // namespace

class AgentOperationIntegrity : public QObject {
  Q_OBJECT
private slots:
  void privateDirectoryBoundaries();
  void managedJournalRejectsDirectoryAlias();
  void restartPreservesIdentityAndLegacyAttemptForm();
  void tamperedOperationIdIsQuarantinedNotSecondBusiness();
  void committedNeighborSurvivesUnsafeRecord();
  void duplicateIdentitiesAreAmbiguous_data();
  void duplicateIdentitiesAreAmbiguous();
  void identitylessCorruptionBlocksWrites();
  void corruptEventRevisionCannotStallPaging();
  void truncatedOrCorruptJournalIsUnavailableNotMissing();
  void workerRefusesQuarantinedIdentityWithoutSideEffects();
  void externalAttachRefusesAliasedOperationDirectory();
  void v1RecordsStayLocalAndInvisibleToAgent();
  void saveDurableIsBlockedWhileNeighborsAreQuarantined();
};

void AgentOperationIntegrity::privateDirectoryBoundaries() {
  QTemporaryDir root;
  const QString op = QStringLiteral("op-") + uuid();
  const QString attempt = uuid();
  QDir(root.path()).mkpath(logDir(root.path()));
  // 快乐路径：裸 UUID 与 attempt-UUID fixture、result 叶子、幂等重入。
  const auto made = operation_detail::createPrivateAttemptDir(root.path(), op, attempt);
  QCOMPARE(made.code, QString());
  QCOMPARE(made.path, QDir(logDir(root.path())).filePath(op + '/' + attempt));
  const auto legacy = operation_detail::createPrivateAttemptDir(root.path(), op, QStringLiteral("attempt-") + uuid());
  QCOMPARE(legacy.code, QString());
  const auto leaf = operation_detail::createPrivateAttemptDir(root.path(), op, attempt,
      QStringLiteral("result-") + uuid());
  QCOMPARE(leaf.code, QString());
  QVERIFY(leaf.path.startsWith(made.path + '/'));
  QCOMPARE(operation_detail::createPrivateAttemptDir(root.path(), op, attempt).path, made.path);
  // 前缀段边界：两条规范 operation 各得各的空间，互不认领（op-a/op-ab
  // 的字符串前缀关系由 operationnamespace 的段精确判定结构性排除）。
  const QString other = QStringLiteral("op-") + uuid();
  const auto otherDir = operation_detail::createPrivateAttemptDir(root.path(), other, attempt);
  QCOMPARE(otherDir.code, QString());
  QVERIFY(otherDir.path != made.path);
  QVERIFY(!otherDir.path.startsWith(made.path + '/'));
  QVERIFY(!made.path.startsWith(otherDir.path + '/'));
  // 身份不合规在任何文件系统动作之前拒绝。
  for (const auto &bad : {QStringLiteral("op-a/../b"), QStringLiteral("/tmp/") + op,
                          QStringLiteral("op-a/b"), QStringLiteral("")})
    QCOMPARE(operation_detail::createPrivateAttemptDir(root.path(), bad, attempt).code,
             QStringLiteral("invalid_operation_identity"));
  QCOMPARE(operation_detail::createPrivateAttemptDir(root.path(), op, "../escape").code,
           QStringLiteral("invalid_operation_identity"));
  QCOMPARE(operation_detail::createPrivateAttemptDir(root.path(), op, attempt, "result-not-a-uuid").code,
           QStringLiteral("invalid_operation_identity"));
  // 根不可用与别名/非目录段分别报告。
  QCOMPARE(operation_detail::createPrivateAttemptDir(root.filePath("missing-root"), op, attempt).code,
           QStringLiteral("operation_root_unavailable"));
  {
    QTemporaryDir other;
    const QString outside = other.filePath("outside");
    QVERIFY(QDir().mkpath(outside));
    // 文件占位与链接别名必须用规范形态 id 才能抵达文件系统走查。
    const QString occupied = QStringLiteral("op-") + uuid();
    const QString linked = QStringLiteral("op-") + uuid();
    QVERIFY(QFile(QDir(logDir(root.path())).filePath(occupied)).open(QIODevice::WriteOnly));
    QCOMPARE(operation_detail::createPrivateAttemptDir(root.path(), occupied, attempt).code,
        QStringLiteral("unsafe_operation_directory"));
    QFile::remove(QDir(logDir(root.path())).filePath(occupied));
    // 链接别名：中间段是 symlink 时拒绝进入他人空间。
    QVERIFY(QFile::link(outside, QDir(logDir(root.path())).filePath(linked)));
    const auto viaLink = operation_detail::createPrivateAttemptDir(root.path(), linked, attempt);
    QCOMPARE(viaLink.code, QStringLiteral("unsafe_operation_directory"));
    QVERIFY(!QFileInfo::exists(QDir(outside).filePath(attempt)));
  }
  // 创建失败（父目录只读）与输入校验错误区分。
  {
    QTemporaryDir locked;
    const QString control = QStringLiteral("op-") + uuid();
    QVERIFY(QDir().mkpath(logDir(locked.path())));
    QFile journal(journalPath(locked.path()));
    QVERIFY(journal.open(QIODevice::WriteOnly));
    journal.close();
    QVERIFY(QFile::setPermissions(logDir(locked.path()), QFile::ReadOwner | QFile::ExeOwner));
    const auto failed = operation_detail::createPrivateAttemptDir(locked.path(), control, uuid());
    QVERIFY(failed.code == QStringLiteral("stage_failed") || failed.code == QString());
    QFile::setPermissions(logDir(locked.path()),
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
  }
}

void AgentOperationIntegrity::managedJournalRejectsDirectoryAlias() {
  QTemporaryDir root;
  QVERIFY(root.isValid());
  const auto target = root.filePath("other-operations");
  const auto alias = logDir(root.path());
  QVERIFY(QDir().mkpath(target));
  QVERIFY(QDir().mkpath(QFileInfo(alias).absolutePath()));
  QVERIFY(linkDirectory(target, alias));
  const auto unlink = qScopeGuard([&] {
#ifdef Q_OS_WIN
    QDir().rmdir(alias);
#else
    QFile::remove(alias);
#endif
  });

  OperationLog unbound(target);
  const auto legacy = unbound.accept("doc", "local", "other", QString(64, 'a'));
  QCOMPARE(legacy.code, QStringLiteral("created"));
  const auto path = QDir(target).filePath("operations.json");
  const auto original = readFileBytes(path);
  QVERIFY(!original.isEmpty());

  ProjectDirLock owner(root.path());
  QVERIFY(owner.tryLock());
  OperationLog log(alias, root.path(), owner.claim());
  const auto op = QStringLiteral("op-") + uuid();
  const auto attempt = uuid();
  QCOMPARE(log.lookupRecordById(legacy.record.operationId).code, QStringLiteral("operation_log_unavailable"));
  QCOMPARE(log.acceptDurable(seed("new")).code, QStringLiteral("project_write_ownership_required"));
  QCOMPARE(operation_detail::createPrivateAttemptDir(root.path(), op, attempt).code,
           QStringLiteral("unsafe_operation_directory"));
  QVERIFY(!QFileInfo::exists(QDir(target).filePath(op)));
  QCOMPARE(readFileBytes(path), original);
}

void AgentOperationIntegrity::restartPreservesIdentityAndLegacyAttemptForm() {
  QTemporaryDir root;
  QString operationId;
  OperationLog::Record running;
  {
    ProjectDirLock owner(root.path());
    QVERIFY(owner.tryLock());
    OperationLog log(logDir(root.path()), root.path(), owner.claim());
    const auto accepted = log.acceptDurable(seed("legacy-attempt"));
    QCOMPARE(accepted.code, QStringLiteral("created"));
    running = accepted.record;
    QCOMPARE(begin(log, running, "attempt-"), QStringLiteral("ok")); // 已发布控制 fixture 形态
    operationId = running.operationId;
  }
  // 跨实例重启：身份、requestHash、key、receipt 语义字段与事件均不变。
  OperationLog reopened(logDir(root.path()), root.path(), {});
  const auto found = reopened.lookupRecordById(operationId);
  QCOMPARE(found.code, QStringLiteral("found"));
  QCOMPARE(found.record.operationId, running.operationId);
  QCOMPARE(found.record.requestHash, running.requestHash);
  QCOMPARE(found.record.key, running.key);
  QCOMPARE(found.record.ownerAttempt, running.ownerAttempt);
  QCOMPARE(found.record.stateRevision, running.stateRevision);
  QCOMPARE(reopened.journalHealth().code, QStringLiteral("ok"));
  // 重启后 running 且无活 attempt → reconcile 落 interrupted；随后 Resume
  // 换裸 UUID attempt（生产形态）依旧受 CAS 保护。
  ProjectDirLock owner(root.path());
  QVERIFY(owner.tryLock());
  OperationLog log(logDir(root.path()), root.path(), owner.claim());
  auto interrupted = found.record;
  interrupted.execution = QStringLiteral("interrupted"); interrupted.code = QStringLiteral("interrupted");
  OperationLog::Record current;
  QCOMPARE(log.saveDurable(interrupted, found.record.stateRevision, found.record.ownerAttempt, &current,
      OperationLog::Transition::Reconcile), QStringLiteral("ok"));
  auto resume = current;
  const auto legacy = resume.ownerAttempt;
  resume.ownerAttempt = uuid();
  resume.ownerProcess = QString::number(QCoreApplication::applicationPid());
  resume.execution = QStringLiteral("running"); resume.code = QStringLiteral("running");
  ++resume.computeCount;
  QCOMPARE(log.saveDurable(resume, current.stateRevision, legacy, &current,
      OperationLog::Transition::Resume), QStringLiteral("ok"));
  QCOMPARE(log.lookupRecordById(operationId).record.ownerAttempt, resume.ownerAttempt);
}

void AgentOperationIntegrity::tamperedOperationIdIsQuarantinedNotSecondBusiness() {
  QTemporaryDir root;
  QString operationId;
  {
    ProjectDirLock owner(root.path());
    QVERIFY(owner.tryLock());
    OperationLog log(logDir(root.path()), root.path(), owner.claim());
    const auto accepted = log.acceptDurable(seed("tampered"));
    QCOMPARE(accepted.code, QStringLiteral("created"));
    operationId = accepted.record.operationId;
  }
  const auto before = readFileBytes(journalPath(root.path()));
  QByteArray beforeRewrite;
  QVERIFY(rewriteJournal(root.path(), [operationId](QJsonArray &rows) {
    for (auto row : rows) {
      auto object = row.toObject();
      if (object.value("operationId").toString() != operationId) continue;
      object.insert("operationId", QStringLiteral("../escape")); // key/requestHash 未动
      row = object;
    }
  }, beforeRewrite));
  const auto tampered = readFileBytes(journalPath(root.path()));
  {
    ProjectDirLock owner(root.path());
    QVERIFY(owner.tryLock());
    const auto treeBefore = treeSnapshot(root.path());
    OperationLog log(logDir(root.path()), root.path(), owner.claim());
    // 目标身份被隔离：lookup 与幂等入口都不是 not found，也不得二次立项。
    QCOMPARE(log.lookup("doc", "agent:g4", "tampered").code, QStringLiteral("operation_record_quarantined"));
    QCOMPARE(log.acceptDurable(seed("tampered")).code, QStringLiteral("operation_record_quarantined"));
    QCOMPARE(log.lookupRecordById(operationId).code, QStringLiteral("operation_not_found"));
    QCOMPARE(log.journalHealth().code, QStringLiteral("operation_log_quarantined"));
    QVERIFY(log.journalHealth().quarantined >= 1);
    // 无损：原始文件字节未被任何入口改写。
    QCOMPARE(readFileBytes(journalPath(root.path())), tampered);
    QVERIFY(readFileBytes(journalPath(root.path())) != before);
    QCOMPARE(treeSnapshot(root.path()), treeBefore);
  } // 锁与 claim 随作用域释放，Host 才能拿到工程写锁
  Host host(root.path());
  QCOMPARE(host.recovery.get("agent:g4", operationId).value("code").toString(),
           QStringLiteral("operation_not_found"));
  QString error;
  QVERIFY(!host.recovery.start("agent:g4", operationId, &host.recovery, "x", {}, false, &error));
  QCOMPARE(error, QStringLiteral("operation_not_found"));
  QCOMPARE(host.recovery.lookup("agent:g4", "tampered").code, QStringLiteral("operation_record_quarantined"));
}

void AgentOperationIntegrity::committedNeighborSurvivesUnsafeRecord() {
  QTemporaryDir root;
  auto calls = std::make_shared<int>(0);
  QString good, bad;
  {
    Host host(root.path());
    const auto a = host.recovery.accept(plan("good", countingWorker(calls)));
    QVERIFY2(a.ok, qPrintable(a.code));
    ExecuteResult result; QString error;
    QVERIFY2(runToEnd(host, a.record, result, &error), qPrintable(error));
    QVERIFY2(result.ok, qPrintable(result.code));
    QCOMPARE(result.commitState, QStringLiteral("committed"));
    good = a.record.operationId;
    const auto b = host.recovery.accept(plan("bad", countingWorker(calls)));
    QVERIFY2(b.ok, qPrintable(b.code));
    bad = b.record.operationId;
  }
  QByteArray tampered;
  QVERIFY(rewriteJournal(root.path(), [bad](QJsonArray &rows) {
    for (auto row : rows) {
      auto object = row.toObject();
      if (object.value("operationId").toString() != bad) continue;
      object.insert("ownerAttempt", QStringLiteral("../sneak"));
      row = object;
    }
  }, tampered));
  Host host(root.path());
  // 合法邻居的 committed 事实不被另一条坏记录抹掉。
  const auto committed = host.recovery.get("agent:g4", good);
  QCOMPARE(committed.value("commitState").toString(), QStringLiteral("committed"));
  QVERIFY(committed.contains("commitReceiptRef"));
  const auto cancelled = host.recovery.cancel("agent:g4", good);
  QCOMPARE(cancelled.value("code").toString(), QStringLiteral("cancel_after_commit"));
  QVERIFY(host.recovery.results("agent:g4", good).value("ports").toArray().size() == 1);
  // 危险记录不可执行且错误可区分。
  QCOMPARE(host.recovery.get("agent:g4", bad).value("code").toString(),
           QStringLiteral("operation_record_quarantined"));
  QString error;
  QVERIFY(!host.recovery.start("agent:g4", bad, &host.recovery, "x", {}, false, &error));
  QCOMPARE(error, QStringLiteral("operation_record_quarantined"));
  QCOMPARE(*calls, 1); // 只有 good 的 worker 真正跑过一次
  // 无损 + retention 不宣称完整。
  QCOMPARE(readFileBytes(journalPath(root.path())), tampered);
  const auto retention = storage::operationRetention(root.path());
  QVERIFY(!retention.complete);
}

void AgentOperationIntegrity::duplicateIdentitiesAreAmbiguous_data() {
  QTest::addColumn<QString>("corruptedIdentity");
  QTest::newRow("valid-duplicate") << QString();
  QTest::newRow("duplicate-operation-id") << QStringLiteral("key");
  QTest::newRow("duplicate-request-identity") << QStringLiteral("operationId");
}

void AgentOperationIntegrity::duplicateIdentitiesAreAmbiguous() {
  QFETCH(QString, corruptedIdentity);
  QTemporaryDir root;
  QString operationId, neighbor;
  {
    ProjectDirLock owner(root.path());
    QVERIFY(owner.tryLock());
    OperationLog log(logDir(root.path()), root.path(), owner.claim());
    const auto first = log.acceptDurable(seed("dup"));
    const auto other = log.acceptDurable(seed("neighbor"));
    QVERIFY(first.code == QStringLiteral("created") && other.code == QStringLiteral("created"));
    operationId = first.record.operationId;
    neighbor = other.record.operationId;
  }
  QByteArray tampered;
  QVERIFY(rewriteJournal(root.path(), [operationId, corruptedIdentity](QJsonArray &rows) {
    for (const auto &row : rows) {
      auto duplicate = row.toObject();
      if (duplicate.value("operationId").toString() != operationId) continue;
      if (!corruptedIdentity.isEmpty()) {
        duplicate.insert(corruptedIdentity, 7);
        duplicate.insert("key", 7); // The v2 identity tuple may still identify the request.
        if (corruptedIdentity == QLatin1String("key"))
          duplicate.remove("documentId"); // Here only the operation ID identifies the duplicate.
      }
      rows.append(duplicate);
      break;
    }
  }, tampered));
  ProjectDirLock owner(root.path());
  QVERIFY(owner.tryLock());
  OperationLog log(logDir(root.path()), root.path(), owner.claim());
  QCOMPARE(log.lookup("doc", "agent:g4", "dup").code, QStringLiteral("operation_record_quarantined"));
  QCOMPARE(log.acceptDurable(seed("dup")).code, QStringLiteral("operation_record_quarantined"));
  QCOMPARE(log.lookupRecordById(operationId).code, QStringLiteral("operation_record_quarantined"));
  // 无冲突邻居照常。
  QCOMPARE(log.lookup("doc", "agent:g4", "neighbor").code, QStringLiteral("found"));
  QCOMPARE(log.lookupRecordById(neighbor).code, QStringLiteral("found"));
  QCOMPARE(log.journalHealth().quarantined, 2);
  QCOMPARE(readFileBytes(journalPath(root.path())), tampered);
}

void AgentOperationIntegrity::identitylessCorruptionBlocksWrites() {
  QTemporaryDir root;
  ProjectDirLock owner(root.path());
  QVERIFY(owner.tryLock());
  OperationLog log(logDir(root.path()), root.path(), owner.claim());
  const auto existing = log.acceptDurable(seed("existing"));
  QCOMPARE(existing.code, QStringLiteral("created"));

  QByteArray tampered;
  QVERIFY(rewriteJournal(root.path(), [](QJsonArray &rows) {
    rows.append(false);
    rows.append(QJsonObject{{"operationId", 3}, {"key", QJsonValue::Null}});
  }, tampered));

  QCOMPARE(log.journalHealth().code, QStringLiteral("operation_log_quarantined"));
  QCOMPARE(log.journalHealth().quarantined, 2);
  QCOMPARE(log.lookupRecordById(existing.record.operationId).code, QStringLiteral("found"));
  QCOMPARE(log.acceptDurable(seed("existing")).code, QStringLiteral("replay"));
  QCOMPARE(log.acceptDurable(seed("new")).code, QStringLiteral("operation_log_quarantined"));
  QCOMPARE(log.accept("doc", "local", "new", QString(64, 'a')).code,
           QStringLiteral("operation_log_quarantined"));
  auto proposal = existing.record;
  proposal.cancelRequested = true;
  QCOMPARE(update(log, proposal), QStringLiteral("operation_log_quarantined"));
  QCOMPARE(readFileBytes(journalPath(root.path())), tampered);
}

void AgentOperationIntegrity::corruptEventRevisionCannotStallPaging() {
  QTemporaryDir root;
  Host host(root.path());
  const auto accepted = host.recovery.accept(plan("events", countingWorker(std::make_shared<int>(0))));
  QVERIFY2(accepted.ok, qPrintable(accepted.code));
  const auto &record = accepted.record;

  QByteArray tampered;
  QVERIFY(rewriteJournal(root.path(), [](QJsonArray &rows) {
    auto row = rows.first().toObject();
    auto events = row.value("events").toArray();
    auto first = events.first().toObject();
    first.insert("revision", "0");
    events.replace(0, first);
    row.insert("events", events);
    rows.replace(0, row);
  }, tampered));
  const auto page = host.recovery.events(record.callerScope, record.operationId, 0, 4, 1024);
  QVERIFY(!page.value("ok").toBool());
  QCOMPARE(page.value("code").toString(), QStringLiteral("operation_record_quarantined"));
  QCOMPARE(readFileBytes(journalPath(root.path())), tampered);
}

void AgentOperationIntegrity::truncatedOrCorruptJournalIsUnavailableNotMissing() {
  QTemporaryDir root;
  {
    ProjectDirLock owner(root.path());
    QVERIFY(owner.tryLock());
    OperationLog log(logDir(root.path()), root.path(), owner.claim());
    QCOMPARE(log.acceptDurable(seed("truncated")).code, QStringLiteral("created"));
  }
  QFile file(journalPath(root.path()));
  QVERIFY(file.open(QIODevice::WriteOnly));
  const QByteArray truncated = "{\"op\":";
  QCOMPARE(file.write(truncated), truncated.size());
  file.close();
  {
    ProjectDirLock owner(root.path());
    QVERIFY(owner.tryLock());
    const auto treeBefore = treeSnapshot(root.path());
    OperationLog log(logDir(root.path()), root.path(), owner.claim());
    // 整本不可读 ≠ 空 journal ≠ not found；同一业务不得被再次接受。
    QCOMPARE(log.lookup("doc", "agent:g4", "truncated").code, QStringLiteral("operation_log_unavailable"));
    QCOMPARE(log.acceptDurable(seed("truncated")).code, QStringLiteral("operation_log_unavailable"));
    QCOMPARE(log.journalHealth().code, QStringLiteral("operation_log_unavailable"));
    QCOMPARE(readFileBytes(journalPath(root.path())), truncated);
    QCOMPARE(treeSnapshot(root.path()), treeBefore);
  } // 锁与 claim 随作用域释放
  Host host(root.path());
  QCOMPARE(host.recovery.get("agent:g4", "anything").value("code").toString(),
           QStringLiteral("operation_log_unavailable"));
  QString error;
  QVERIFY(!host.recovery.start("agent:g4", "anything", &host.recovery, "x", {}, false, &error));
  QCOMPARE(error, QStringLiteral("operation_log_unavailable"));
}

void AgentOperationIntegrity::workerRefusesQuarantinedIdentityWithoutSideEffects() {
  QTemporaryDir root;
  auto calls = std::make_shared<int>(0);
  QString operationId;
  {
    Host host(root.path());
    const auto a = host.recovery.accept(plan("worker-refused", countingWorker(calls)));
    QVERIFY2(a.ok, qPrintable(a.code));
    operationId = a.record.operationId;
  }
  QByteArray ignored;
  QVERIFY(rewriteJournal(root.path(), [operationId](QJsonArray &rows) {
    for (auto row : rows) {
      auto object = row.toObject();
      if (object.value("operationId").toString() != operationId) continue;
      object.insert("operationId", QStringLiteral("op-a/../../escape"));
      row = object;
    }
  }, ignored));
  Host host(root.path());
  const auto treeBefore = treeSnapshot(root.path()); // catalog.open 之后取基线
  QString error;
  QVERIFY(!host.recovery.start("agent:g4", operationId, &host.recovery, "x", {}, false, &error));
  QCOMPARE(error, QStringLiteral("operation_not_found"));
  QCOMPARE(*calls, 0);
  QCOMPARE(treeSnapshot(root.path()), treeBefore); // 未创建任何私有目录
  // 原 key 的幂等入口同样被隔离挡住。
  QCOMPARE(host.recovery.lookup("agent:g4", "worker-refused").code,
           QStringLiteral("operation_record_quarantined"));
}

void AgentOperationIntegrity::externalAttachRefusesAliasedOperationDirectory() {
  QTemporaryDir root;
  QString operationId, attempt;
  {
    Host host(root.path());
    host.allowExternal = true;
    auto calls = std::make_shared<int>(0);
    auto p = plan("external-attach", countingWorker(calls));
    p.phase = "compute-only"; p.outputSpec = {}; p.outputPorts.clear();
    p.externalRequest = {{"requestHash", QString(64, 'a')}, {"modelVersion", "v1"}};
    const auto a = host.recovery.beginExternal(p);
    QVERIFY2(a.ok, qPrintable(a.code));
    operationId = a.record.operationId;
    attempt = a.record.ownerAttempt;
    QCOMPARE(host.recovery.noteRemoteAccepted("agent:g4", operationId, "job-1"), QStringLiteral("ok"));
    const QJsonObject proof{{"requestHash", QString(64, 'a')}, {"responseHash", QString(64, 'b')},
        {"modelVersion", "v1"}};
    QCOMPARE(host.recovery.reportExternal("agent:g4", operationId, "job-1", "succeeded", proof, true),
        QStringLiteral("ok"));
  }
  // 把 operation 目录换成指向工程外真实目录的符号链接：附件必须拒绝进入
  // 他人空间，不调用 worker，不落 manifest。
  QTemporaryDir outside;
  QVERIFY(QDir().mkpath(outside.filePath("stolen")));
  const QString opDir = QDir(logDir(root.path())).filePath(operationId);
  QVERIFY(QDir().mkpath(opDir + '/' + attempt));
  QVERIFY(QDir(opDir).rmdir(attempt)); // 撤空 attempt 与 op 目录，换成链接
  QVERIFY(QDir().rmdir(opDir));
  QVERIFY(QFile::link(outside.filePath("stolen"), opDir));
  Host host(root.path());
  int calls = 0;
  QString error; bool done = false; ExecuteResult result;
  auto *task = host.recovery.attachExternalResults("agent:g4", operationId,
      [&calls](const OperationWorkContext &) { ++calls; return OperationWorkResult{}; },
      &host.recovery, [&](ExecuteResult r) { result = r; done = true; }, &error);
  QVERIFY2(task, qPrintable(error));
  QEventLoop loop;
  QObject::connect(task, &PaleoTask::finished, &loop, &QEventLoop::quit);
  loop.exec();
  QTRY_VERIFY(done);
  QVERIFY(!result.ok);
  QCOMPARE(result.code, QStringLiteral("unsafe_operation_directory"));
  QCOMPARE(calls, 0);
  QVERIFY(!QFileInfo::exists(QDir(outside.filePath("stolen")).filePath("result")));
  OperationLog log(logDir(root.path()));
  QVERIFY(log.recordById(operationId).artifactManifest.isEmpty());
}

void AgentOperationIntegrity::v1RecordsStayLocalAndInvisibleToAgent() {
  QTemporaryDir root;
  QString v1Id, v2Id;
  const auto legacyPlan = plan("legacy", {});
  {
    ProjectDirLock owner(root.path());
    QVERIFY(owner.tryLock());
    OperationLog log(logDir(root.path()), root.path(), owner.claim());
    const auto v1 = log.accept("doc", "agent:g4", "legacy", operationDigest(legacyPlan.intent));
    QCOMPARE(v1.code, QStringLiteral("created"));
    v1Id = v1.record.operationId;
    const auto v2 = log.acceptDurable(seed("durable"));
    QCOMPARE(v2.code, QStringLiteral("created"));
    v2Id = v2.record.operationId;
    // v1 遗留 advance 契约照常。
    auto running = v1.record;
    running.execution = "running"; ++running.computeCount;
    OperationLog::Record current;
    QVERIFY(log.save(running, &current));
  }
  OperationLog reader(logDir(root.path()));
  QCOMPARE(reader.lookupRecordById(v1Id).code, QStringLiteral("found"));
  QCOMPARE(reader.lookupRecordById(v2Id).code, QStringLiteral("found"));
  // Agent 面：v1 缺 v2 document/caller scope 绑定，get/results/read/resume
  // 一律不可见——安全 ID 形态不授予越过 v2 授权的任何权限。
  Host host(root.path());
  const auto lookup = host.recovery.lookup("agent:g4", "legacy");
  QCOMPARE(lookup.code, QStringLiteral("legacy_unverifiable"));
  QVERIFY(lookup.record.operationId.isEmpty());
  const auto replay = host.recovery.accept(legacyPlan);
  QVERIFY(!replay.ok);
  QCOMPARE(replay.code, QStringLiteral("legacy_unverifiable"));
  QVERIFY(replay.record.operationId.isEmpty());
  QCOMPARE(host.recovery.get("agent:g4", v1Id).value("code").toString(), QStringLiteral("operation_not_found"));
  QString error;
  QVERIFY(!host.recovery.read("agent:g4", v1Id, "OUTPUT", 0, 8, &host.recovery, {}, &error));
  QCOMPARE(error, QStringLiteral("operation_not_found"));
  QVERIFY(!host.recovery.start("agent:g4", v1Id, &host.recovery, "x", {}, false, &error));
  QCOMPARE(error, QStringLiteral("operation_not_found"));
  QCOMPARE(host.recovery.get("agent:g4", v2Id).value("ok").toBool(), true);
}

void AgentOperationIntegrity::saveDurableIsBlockedWhileNeighborsAreQuarantined() {
  QTemporaryDir root;
  OperationLog::Record target;
  {
    ProjectDirLock owner(root.path());
    QVERIFY(owner.tryLock());
    OperationLog log(logDir(root.path()), root.path(), owner.claim());
    const auto a = log.acceptDurable(seed("advance"));
    const auto b = log.acceptDurable(seed("broken"));
    QVERIFY(a.code == QStringLiteral("created") && b.code == QStringLiteral("created"));
    target = a.record;
    QCOMPARE(begin(log, target), QStringLiteral("ok"));
    // 非法 attempt 变更在干净 journal 上就被 owner 契约独立拒绝（与隔离无关）。
    auto evil = target; evil.ownerAttempt = "../pwn";
    QCOMPARE(log.saveDurable(evil, target.stateRevision, target.ownerAttempt),
        QStringLiteral("invalid_operation_owner"));
  }
  QByteArray tampered;
  QVERIFY(rewriteJournal(root.path(), [](QJsonArray &rows) {
    for (auto row : rows) {
      auto object = row.toObject();
      if (object.value("clientRequestId").toString() != "broken") continue;
      object.insert("stateRevision", QStringLiteral("not-a-number"));
      row = object;
    }
  }, tampered));
  ProjectDirLock owner(root.path());
  QVERIFY(owner.tryLock());
  OperationLog log(logDir(root.path()), root.path(), owner.claim());
  auto proposal = target;
  proposal.execution = "prepared"; proposal.preparedManifest = {{"kind", "control"}};
  OperationLog::Record current;
  // CAS 目标可读（current 回传磁盘现状），但写入被无损纪律挡住。
  QCOMPARE(log.saveDurable(proposal, target.stateRevision, target.ownerAttempt, &current),
      QStringLiteral("operation_log_quarantined"));
  QCOMPARE(current.stateRevision, target.stateRevision);
  QCOMPARE(current.execution, QStringLiteral("running"));
  // 隔离在场时任何 proposal（含非法 attempt）统一被无损纪律挡在最前面。
  auto evil = target; evil.ownerAttempt = "../pwn";
  QCOMPARE(log.saveDurable(evil, target.stateRevision, target.ownerAttempt),
      QStringLiteral("operation_log_quarantined"));
  // 磁盘无损：坏邻居与目标记录均保持篡改后的字节。
  QCOMPARE(readFileBytes(journalPath(root.path())), tampered);
  QCOMPARE(log.lookupRecordById(target.operationId).record.execution, QStringLiteral("running"));
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  AgentOperationIntegrity test;
  return QTest::qExec(&test, argc, argv);
}
#include "tst_agent_operation_integrity.moc"
