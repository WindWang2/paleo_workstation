// 层：测试壳
// H4/tst_h4_operation_discovery — 只读发现的生产入口验证：作用域快照、
// 分页/游标预算、关系分类、坏邻居隔离、取消/销毁与零对账副作用。全部经
// 真实持久文件与 Recovery/OperationLog 生产 API；规模夹具用满足 loader
// 严格校验的合成 journal 行（不执行数千次科学计算），oracle 用稳定 ID
// 集合、扫描计数、缓存保留字节与 journal 原始 SHA。
#include <QtTest>
#include "workflow/capabilityrecovery.h"
#include "workflow/operationdiscovery.h"
#include "workflow/capabilityops_internal.h"
#include "catalog/datacatalog.h"
#include "metadata/projectlock.h"
#include "services/paleotaskservice.h"
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

using namespace paleo;
using namespace paleo::capability;

namespace {
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString logDir(const QString &root) { return QDir(root).filePath(".paleo/capability-ops"); }
QString journalPath(const QString &root) { return QDir(logDir(root)).filePath("operations.json"); }

QByteArray fileSha(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

OperationLog::Record seed(const QString &request, const QString &scope = "agent:h4") {
  OperationLog::Record record;
  record.documentId = "doc";
  record.callerScope = scope;
  record.clientRequestId = request;
  record.capabilityId = "h4.compute";
  record.implementationVersion = "1";
  record.requestHash = QString(64, 'a');
  record.planHash = QString(64, 'b');
  record.frozenRequest = {{"intent", "h4"}};
  return record;
}
QString begin(OperationLog &log, OperationLog::Record &record) {
  const auto oldAttempt = record.ownerAttempt;
  record.ownerAttempt = uuid();
  record.ownerProcess = QString::number(QCoreApplication::applicationPid());
  record.execution = "running";
  record.code = "running";
  ++record.computeCount;
  OperationLog::Record current;
  const auto result = log.saveDurable(record, record.stateRevision, oldAttempt, &current);
  if (result == QLatin1String("ok"))
    record = current;
  return result;
}
QString update(OperationLog &log, OperationLog::Record &record) {
  OperationLog::Record current;
  const auto result = log.saveDurable(record, record.stateRevision, record.ownerAttempt, &current);
  if (result == QLatin1String("ok"))
    record = current;
  return result;
}

OperationWorker countingWorker(std::shared_ptr<int> calls) {
  return [calls](const OperationWorkContext &c) {
    ++*calls;
    OperationWorkResult r;
    QFile file(c.privateDir + "/values.bin");
    r.ok = file.open(QIODevice::WriteOnly) && file.write("h4-worker-bytes") == 15;
    file.close();
    r.outputs.ok = r.ok;
    r.outputs.artifacts.append({"OUTPUT", file.fileName(), {}, {}, {}, {}});
    return r;
  };
}
FrozenOperation plan(const QString &request, const OperationWorker &worker, const QString &scope = "agent:h4",
                     const QString &sourceOperationId = QString()) {
  FrozenOperation p;
  p.capabilityId = "h4.compute";
  p.implementationVersion = "1";
  p.callerScope = scope;
  p.clientRequestId = request;
  p.intent = {{"number", 1}};
  p.frozenPlan = {{"immutable", true}};
  p.phase = "publish";
  p.worker = worker;
  p.sourceOperationId = sourceOperationId;
  p.outputSpec.id = "h4:values";
  p.outputSpec.complete = true;
  AlgoPort port;
  port.id = "OUTPUT";
  port.kind = "file";
  p.outputSpec.outputs = {port};
  p.outputPorts = {"OUTPUT"};
  return p;
}

struct Host {
  DataCatalog catalog;
  std::unique_ptr<ProjectDirLock> lock;
  PaleoTaskService tasks;
  CapabilityRecovery recovery;
  QString documentId = "doc";
  unsigned allowedEffects = host::Read | host::PrivateStage | host::CatalogPublish | host::Presentation;
  bool ownsWrites = true;
  explicit Host(const QString &root) {
    lock = std::make_unique<ProjectDirLock>(root);
    QVERIFY2(lock->tryLock(), "project lock");
    QVERIFY2(catalog.open(root), "catalog open");
    recovery.setTaskService(&tasks);
    recovery.setHostProvider([this, root] {
      RecoveryHost h;
      h.projectDir = root;
      h.operationDir = logDir(root);
      h.catalog = &catalog;
      h.writeClaim = lock->claim();
      h.context.documentId = documentId;
      h.context.epoch = 1;
      h.context.open = true;
      h.context.persistentIdentity = true;
      h.context.readOnly = false;
      h.context.ownsWrites = ownsWrites;
      h.context.allowedEffects = allowedEffects;
      return h;
    });
  }
};

bool runToEnd(Host &host, const OperationLog::Record &record, ExecuteResult &result, QString *error) {
  bool done = false;
  error->clear();
  auto *task = host.recovery.start(record.callerScope, record.operationId, &host.recovery, "h4",
      [&](ExecuteResult r) { result = r; done = true; }, false, error);
  if (!task)
    return false;
  QEventLoop loop;
  QObject::connect(task, &PaleoTask::finished, &loop, &QEventLoop::quit);
  loop.exec();
  for (int i = 0; i < 500 && !done; ++i) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  return done;
}

bool listOnce(CapabilityRecovery &recovery, const QString &scope, const QJsonObject &args, QJsonObject &out,
              QString *error) {
  bool done = false;
  error->clear();
  auto *task = recovery.listOperations(scope, args, &recovery, [&](QJsonObject r) { out = r; done = true; }, error);
  if (!task)
    return false;
  QEventLoop loop;
  QObject::connect(task, &PaleoTask::finished, &loop, &QEventLoop::quit);
  loop.exec();
  for (int i = 0; i < 500 && !done; ++i) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  return done;
}

// 合成一条满足 readRecord 严格校验的 v2 journal 行（规模/关系夹具用）。
QJsonObject syntheticRow(const QString &doc, const QString &scope, const QString &request,
                         const QJsonObject &frozenRequest = QJsonObject(),
                         const QJsonObject &artifactManifest = QJsonObject(),
                         const QJsonObject &stagePlan = QJsonObject()) {
  QJsonObject row;
  row.insert(QStringLiteral("operationId"), QStringLiteral("op-") + uuid());
  row.insert(QStringLiteral("key"), operation_detail::key(doc, scope, request));
  row.insert(QStringLiteral("requestHash"), QString(64, 'c'));
  row.insert(QStringLiteral("execution"), QStringLiteral("succeeded"));
  row.insert(QStringLiteral("commitState"), QStringLiteral("committed"));
  row.insert(QStringLiteral("presentation"), QStringLiteral("presented"));
  row.insert(QStringLiteral("code"), QStringLiteral("committed"));
  row.insert(QStringLiteral("computeCount"), 1);
  row.insert(QStringLiteral("summary"), QString::fromLatin1(QByteArrayLiteral("{}").toBase64()));
  row.insert(QStringLiteral("schemaVersion"), 2);
  row.insert(QStringLiteral("documentId"), doc);
  row.insert(QStringLiteral("callerScope"), scope);
  row.insert(QStringLiteral("clientRequestId"), request);
  row.insert(QStringLiteral("capabilityId"), QStringLiteral("h4.bulk"));
  row.insert(QStringLiteral("contractVersion"), 1);
  row.insert(QStringLiteral("implementationVersion"), QStringLiteral("1"));
  row.insert(QStringLiteral("planHash"), QString(64, 'd'));
  row.insert(QStringLiteral("storeGeneration"), QStringLiteral("gen"));
  row.insert(QStringLiteral("stateRevision"), QStringLiteral("1"));
  row.insert(QStringLiteral("ownerAttempt"), QString());
  row.insert(QStringLiteral("ownerProcess"), QStringLiteral("1"));
  row.insert(QStringLiteral("cancelRequested"), false);
  row.insert(QStringLiteral("frozenRequest"),
             frozenRequest.isEmpty() ? QJsonObject{{QStringLiteral("intent"), QStringLiteral("bulk")}} : frozenRequest);
  row.insert(QStringLiteral("stagePlan"), stagePlan);
  row.insert(QStringLiteral("preparedManifest"), QJsonObject{});
  row.insert(QStringLiteral("artifactManifest"), artifactManifest);
  row.insert(QStringLiteral("presentationIntent"), QJsonObject{});
  row.insert(QStringLiteral("externalRequest"), QJsonObject{});
  row.insert(QStringLiteral("remoteJobId"), QString());
  row.insert(QStringLiteral("externalOutcome"), QString());
  row.insert(QStringLiteral("remoteCompletionProof"), QJsonObject{});
  row.insert(QStringLiteral("events"),
             QJsonArray{QJsonObject{{QStringLiteral("revision"), QStringLiteral("1")},
                                    {QStringLiteral("execution"), QStringLiteral("succeeded")},
                                    {QStringLiteral("commitState"), QStringLiteral("committed")},
                                    {QStringLiteral("presentation"), QStringLiteral("presented")},
                                    {QStringLiteral("code"), QStringLiteral("committed")},
                                    {QStringLiteral("attempt"), QString()},
                                    {QStringLiteral("computeCount"), 1},
                                    {QStringLiteral("cancelRequested"), false},
                                    {QStringLiteral("time"), QStringLiteral("2026-11-23T00:00:00.000Z")}}});
  return row;
}

QStringList operationIds(const QJsonObject &page) {
  QStringList ids;
  for (const auto &entry : page.value(QStringLiteral("operations")).toArray())
    ids.append(entry.toObject().value(QStringLiteral("operationId")).toString());
  return ids;
}
} // namespace

class H4OperationDiscovery : public QObject {
  Q_OBJECT
private slots:
  void discoversScopedOperationsAfterRestart();
  void scopeAndDocumentIsolation();
  void v1QuarantinedAndForeignNeighborsStayOut();
  void emptyAndCorruptJournalsAreDistinguished();
  void bulkJournalPagesWithoutRescan();
  void budgetCursorAndCacheDiscipline();
  void snapshotImmutabilityWhileBusinessAdvances();
  void relationsClassifyDirectTargets();
  void cancelPreservesJournalAndBusinessState();
  void subscriberDestructionKeepsServiceUsable();
  void readGateAndWriteDenialBoundaries();
  void listDoesNotReconcileWhileGetDoes();
};

void H4OperationDiscovery::discoversScopedOperationsAfterRestart() {
  QTemporaryDir root;
  const QString scope = "agent:h4:chat:" + uuid();
  QStringList created;
  {
    Host host(root.path());
    const auto calls = std::make_shared<int>(0);
    for (int i = 0; i < 3; ++i) {
      const auto accepted = host.recovery.accept(plan(QStringLiteral("req-%1").arg(i), countingWorker(calls), scope));
      QVERIFY2(accepted.ok, qPrintable(accepted.code));
      ExecuteResult result;
      QString error;
      QVERIFY(runToEnd(host, accepted.record, result, &error));
      QVERIFY2(result.ok, qPrintable(result.code + QLatin1Char(' ') + error));
      created.append(accepted.record.operationId);
    }
    QCOMPARE(*calls, 3);
  }
  // 冷重启：不依赖聊天历史/内存索引，仅凭 journal 重新发现原 ID。
  Host host(root.path());
  QJsonObject page;
  QString error;
  QVERIFY(listOnce(host.recovery, scope, {}, page, &error));
  QVERIFY2(page.value(QStringLiteral("ok")).toBool(),
           qPrintable(QString::fromUtf8(QJsonDocument(page).toJson(QJsonDocument::Compact))));
  QStringList sorted = created;
  sorted.sort();
  const auto ids = operationIds(page);
  QCOMPARE(ids, sorted); // 快照按 operationId 字典序返回
  QVERIFY(page.value(QStringLiteral("coverage")).toObject().value(QStringLiteral("journalHealth")).toString()
          == QLatin1String("ok"));
  // 发现的 ID 随后可走原 get/results 链（不读聊天结果历史）。
  const auto state = host.recovery.get(scope, ids.first());
  QVERIFY2(state.value(QStringLiteral("ok")).toBool(), qPrintable(state.value(QStringLiteral("code")).toString()));
  QCOMPARE(state.value(QStringLiteral("commitState")).toString(), QStringLiteral("committed"));
  const auto results = host.recovery.results(scope, ids.first());
  QVERIFY2(results.value(QStringLiteral("ok")).toBool(), qPrintable(results.value(QStringLiteral("code")).toString()));
  QCOMPARE(results.value(QStringLiteral("ports")).toArray().size(), 1);
}

void H4OperationDiscovery::scopeAndDocumentIsolation() {
  QTemporaryDir root;
  Host host(root.path());
  QDir().mkpath(logDir(root.path()));
  QJsonArray rows;
  rows.append(syntheticRow("doc", "agent:h4:chat:a", "a1"));
  rows.append(syntheticRow("doc", "agent:h4:chat:a", "a2"));
  rows.append(syntheticRow("doc", "agent:h4:chat:b", "b1"));
  rows.append(syntheticRow("docB", "agent:h4:chat:a", "x1"));
  {
    QFile file(journalPath(root.path()));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(rows).toJson(QJsonDocument::Compact));
  }
  QJsonObject page;
  QString error;
  QVERIFY(listOnce(host.recovery, QStringLiteral("agent:h4:chat:a"), {}, page, &error));
  QVERIFY(page.value(QStringLiteral("ok")).toBool());
  const auto ids = operationIds(page);
  QCOMPARE(ids.size(), 2);
  const auto encoded = QString::fromUtf8(QJsonDocument(page).toJson(QJsonDocument::Compact));
  QVERIFY(!encoded.contains(QLatin1String("chat:b")));
  QVERIFY(!encoded.contains(QLatin1String("docB")));
  QVERIFY(!encoded.contains(rows.at(2).toObject().value(QStringLiteral("operationId")).toString()));
  QVERIFY(!encoded.contains(rows.at(3).toObject().value(QStringLiteral("operationId")).toString()));
  // 换 scope 后旧游标拒绝（不返回其他查询页）：先取有续页的小页。
  QJsonObject paged;
  QVERIFY(listOnce(host.recovery, QStringLiteral("agent:h4:chat:a"),
                   {{QStringLiteral("page_size"), 1}}, paged, &error));
  QVERIFY(paged.value(QStringLiteral("ok")).toBool());
  const QString cursor = paged.value(QStringLiteral("nextCursor")).toString();
  QVERIFY(!cursor.isEmpty());
  QJsonObject other;
  QVERIFY(listOnce(host.recovery, QStringLiteral("agent:h4:chat:b"),
                   {{QStringLiteral("cursor"), cursor}, {QStringLiteral("page_size"), 1}}, other, &error));
  QCOMPARE(other.value(QStringLiteral("code")).toString(), QStringLiteral("cursor_scope_mismatch"));
}

void H4OperationDiscovery::v1QuarantinedAndForeignNeighborsStayOut() {
  QTemporaryDir root;
  Host host(root.path());
  const QString scope = "agent:h4:chat:a";
  const auto calls = std::make_shared<int>(0);
  const auto accepted = host.recovery.accept(plan("legal", countingWorker(calls), scope));
  QVERIFY2(accepted.ok, qPrintable(accepted.code));
  ExecuteResult result;
  QString error;
  QVERIFY(runToEnd(host, accepted.record, result, &error));
  const auto before = fileSha(journalPath(root.path()));
  // v1 邻居 + 重复身份坏行：v1 不进 Agent 清单，坏行触发隔离。
  {
    QFile file(journalPath(root.path()));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto rows = QJsonDocument::fromJson(file.readAll()).array();
    file.close();
    QJsonArray edited = rows;
    QJsonObject v1;
    v1.insert(QStringLiteral("operationId"), QStringLiteral("op-") + uuid());
    v1.insert(QStringLiteral("key"), operation_detail::key("doc", scope, "legacy"));
    v1.insert(QStringLiteral("requestHash"), QString(64, 'e'));
    v1.insert(QStringLiteral("execution"), QStringLiteral("succeeded"));
    v1.insert(QStringLiteral("commitState"), QStringLiteral("not_requested"));
    v1.insert(QStringLiteral("presentation"), QStringLiteral("not_requested"));
    v1.insert(QStringLiteral("code"), QStringLiteral("done"));
    v1.insert(QStringLiteral("computeCount"), 0);
    v1.insert(QStringLiteral("summary"), QString::fromLatin1(QByteArrayLiteral("{}").toBase64()));
    edited.append(v1);
    edited.append(rows.at(0)); // 重复身份 → 双方隔离
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(edited).toJson(QJsonDocument::Compact));
  }
  QJsonObject page;
  QVERIFY(listOnce(host.recovery, scope, {}, page, &error));
  QVERIFY(page.value(QStringLiteral("ok")).toBool());
  // 合法同 scope 邻居被重复身份拖入隔离（G4 无损纪律：同一身份的每条都隔离），
  // 因此清单为空但健康度是 incomplete，不是"全部业务均已发现"。
  QCOMPARE(operationIds(page).size(), 0);
  QCOMPARE(page.value(QStringLiteral("coverage")).toObject().value(QStringLiteral("journalHealth")).toString(),
           QStringLiteral("incomplete"));
  const auto encoded = QString::fromUtf8(QJsonDocument(page).toJson(QJsonDocument::Compact));
  QVERIFY(!encoded.contains(QLatin1String("legacy")));
  // 零写入：原始字节不变。
  QCOMPARE(fileSha(journalPath(root.path())), before);
}

void H4OperationDiscovery::emptyAndCorruptJournalsAreDistinguished() {
  QTemporaryDir root;
  {
    Host host(root.path());
    QJsonObject page;
    QString error;
    QVERIFY(listOnce(host.recovery, "agent:h4:chat:a", {}, page, &error));
    QVERIFY(page.value(QStringLiteral("ok")).toBool());
    QCOMPARE(page.value(QStringLiteral("returnedCount")).toInt(), 0);
    QCOMPARE(page.value(QStringLiteral("coverage")).toObject().value(QStringLiteral("journalHealth")).toString(),
             QStringLiteral("ok"));
  }
  QDir().mkpath(logDir(root.path()));
  {
    QFile file(journalPath(root.path()));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"not\":\"an array\"");
  }
  Host host(root.path());
  QJsonObject page;
  QString error;
  QVERIFY(listOnce(host.recovery, "agent:h4:chat:a", {}, page, &error));
  QVERIFY(!page.value(QStringLiteral("ok")).toBool());
  QCOMPARE(page.value(QStringLiteral("code")).toString(), QStringLiteral("operation_log_unavailable"));
}

void H4OperationDiscovery::bulkJournalPagesWithoutRescan() {
  QTemporaryDir root;
  QDir().mkpath(logDir(root.path()));
  const QString scope = "agent:h4:chat:bulk";
  QJsonArray rows;
  QStringList expected;
  for (int i = 0; i < 4000; ++i) {
    const auto row = syntheticRow("doc", scope, QStringLiteral("bulk-%1").arg(i, 4, 10, QChar('0')));
    expected.append(row.value(QStringLiteral("operationId")).toString());
    rows.append(row);
  }
  expected.sort();
  {
    QFile file(journalPath(root.path()));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(rows).toJson(QJsonDocument::Compact));
  }
  ProjectDirLock lock(root.path());
  QVERIFY(lock.tryLock());
  OperationDiscovery service;
  const DiscoveryScan scan{QFileInfo(root.path()).canonicalFilePath(), logDir(root.path()), "doc", scope,
                           lock.claim()};
  QStringList walked;
  QString cursor;
  int pages = 0;
  forever {
    QJsonObject args{{QStringLiteral("page_size"), 32}, {QStringLiteral("max_bytes"), 24576}};
    if (!cursor.isEmpty())
      args.insert(QStringLiteral("cursor"), cursor);
    const auto page = service.query(scan, args);
    QVERIFY2(page.value(QStringLiteral("ok")).toBool(),
             qPrintable(page.value(QStringLiteral("code")).toString()));
    walked.append(operationIds(page));
    ++pages;
    // 一次快照顺序分页：续页不再全文件解析（扫描计数不涨），字节预算内。
    QCOMPARE(service.scanCount(), 1);
    QVERIFY(QJsonDocument(page).toJson(QJsonDocument::Compact).size() <= 24576);
    // 游标有界：同快照只保留「可重试 + 续页」两个，不随页数增长。
    QVERIFY(service.liveCursors() <= 2);
    cursor = page.value(QStringLiteral("nextCursor")).toString();
    if (cursor.isEmpty())
      break;
    QVERIFY(page.value(QStringLiteral("hasMore")).toBool());
  }
  QCOMPARE(pages, 125); // ceil(4000/32)
  QCOMPARE(walked, expected); // 无重复/遗漏，序 = operationId 字典序
  QVERIFY(service.retainedBytes() > 0);
  QCOMPARE(service.liveSnapshots(), 1);
  QCOMPARE(service.liveCursors(), 1); // 末页后仅剩刚消费的可重试游标
  // 缺省预算（更小页）同样能走完整本：游标回收使长遍历不受 maxCursors 限制。
  OperationDiscovery small;
  QStringList smallWalked;
  QString smallCursor;
  int smallPages = 0;
  forever {
    QJsonObject args;
    if (!smallCursor.isEmpty())
      args.insert(QStringLiteral("cursor"), smallCursor);
    const auto page = small.query(scan, args);
    QVERIFY2(page.value(QStringLiteral("ok")).toBool(),
             qPrintable(page.value(QStringLiteral("code")).toString()));
    smallWalked.append(operationIds(page));
    ++smallPages;
    QCOMPARE(small.scanCount(), 1);
    smallCursor = page.value(QStringLiteral("nextCursor")).toString();
    if (smallCursor.isEmpty())
      break;
  }
  QVERIFY(smallPages > 125); // 缺省 8192-1024 行区 < 32 行/页
  QCOMPARE(smallWalked, expected);
}

void H4OperationDiscovery::budgetCursorAndCacheDiscipline() {
  QTemporaryDir root;
  QDir().mkpath(logDir(root.path()));
  const QString scope = "agent:h4:chat:cursor";
  QJsonArray rows;
  rows.append(syntheticRow("doc", scope, "r1"));
  rows.append(syntheticRow("doc", scope, "r2"));
  {
    QFile file(journalPath(root.path()));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(rows).toJson(QJsonDocument::Compact));
  }
  ProjectDirLock lock(root.path());
  QVERIFY(lock.tryLock());
  const DiscoveryScan scan{QFileInfo(root.path()).canonicalFilePath(), logDir(root.path()), "doc", scope,
                           lock.claim()};
  // 新错误码负例（W1 契约收紧）：view 键出现即须精确词表值；筛选只属于
  // operations 视图，与 relations 组合显式拒绝（先于锚点与磁盘）。
  OperationDiscovery contract;
  QCOMPARE(contract.query(scan, {{QStringLiteral("view"), QString()}})
               .value(QStringLiteral("code")).toString(),
           QStringLiteral("invalid_discovery_view"));
  QCOMPARE(contract.query(scan, {{QStringLiteral("view"), QStringLiteral("relations")},
                                 {QStringLiteral("execution_state"), QStringLiteral("succeeded")}})
               .value(QStringLiteral("code")).toString(),
           QStringLiteral("invalid_filter_for_view"));
  qint64 now = 1000;
  OperationDiscovery service;
  service.setClock([&] { return now; });

  // 单行超页预算：明确 minimumBytes，不截断 ID、不静默丢条。加一条超长
  // requestHash 的行，使该分支在最小预算下必然触发。
  {
    QFile file(journalPath(root.path()));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto rows = QJsonDocument::fromJson(file.readAll()).array();
    file.close();
    auto big = syntheticRow("doc", scope, "big");
    big.insert(QStringLiteral("requestHash"), QString(2000, QChar('x')));
    big.insert(QStringLiteral("capabilityId"), QStringLiteral("h4.big"));
    QJsonArray edited = rows;
    edited.append(big);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(edited).toJson(QJsonDocument::Compact));
  }
  OperationDiscovery tinyRows;
  const auto tooSmall = tinyRows.query(scan, {{QStringLiteral("max_bytes"), 2048},
                                              {QStringLiteral("capability_id"), QStringLiteral("h4.big")},
                                              {QStringLiteral("page_size"), 1}});
  // 过滤只留 big 行：单行即超 2048-1024 行区 → page_budget_too_small。
  QCOMPARE(tooSmall.value(QStringLiteral("code")).toString(), QStringLiteral("page_budget_too_small"));
  QVERIFY(tooSmall.value(QStringLiteral("minimumBytes")).toInt() > 2048);

  QJsonObject page = service.query(scan, {{QStringLiteral("page_size"), 1}});
  QVERIFY(page.value(QStringLiteral("ok")).toBool());
  const QString cursor = page.value(QStringLiteral("nextCursor")).toString();
  QVERIFY(!cursor.isEmpty());

  // 篡改游标（MAC 负例）与换条件续页。
  auto tampered = cursor;
  tampered.replace(tampered.size() - 1, 1, tampered.endsWith(QLatin1Char('0')) ? QLatin1Char('1') : QLatin1Char('0'));
  QCOMPARE(service.query(scan, {{QStringLiteral("cursor"), tampered}}).value(QStringLiteral("code")).toString(),
           QStringLiteral("cursor_invalid"));
  QCOMPARE(service.query(scan, {{QStringLiteral("cursor"), cursor},
                                {QStringLiteral("page_size"), 1},
                                {QStringLiteral("execution_state"), QStringLiteral("succeeded")}})
               .value(QStringLiteral("code")).toString(),
           QStringLiteral("cursor_query_mismatch"));

  // TTL 到期（注入时钟）：cursor_expired，不静默回第一页。
  now += 10 * 60 * 1000 + 1;
  QCOMPARE(service.query(scan, {{QStringLiteral("cursor"), cursor}, {QStringLiteral("page_size"), 1}})
               .value(QStringLiteral("code")).toString(),
           QStringLiteral("cursor_expired"));

  // 缓存满（maxSnapshots=1）：新快照分配结构化失败，仍有效的旧快照分页
  // 不被挤掉——续页照常，不静默丢承诺。
  OperationDiscovery tiny(DiscoveryPolicy{1, 128, 600000, 8 * 1024 * 1024});
  auto first = tiny.query(scan, {{QStringLiteral("page_size"), 1}});
  QVERIFY(first.value(QStringLiteral("ok")).toBool());
  const QString liveCursor = first.value(QStringLiteral("nextCursor")).toString();
  QVERIFY(!liveCursor.isEmpty());
  QCOMPARE(tiny.query(scan, {{QStringLiteral("execution_state"), QStringLiteral("failed")}})
               .value(QStringLiteral("code")).toString(),
           QStringLiteral("discovery_cache_full"));
  const auto continued = tiny.query(scan, {{QStringLiteral("cursor"), liveCursor},
                                           {QStringLiteral("page_size"), 1}});
  QVERIFY2(continued.value(QStringLiteral("ok")).toBool(),
           qPrintable(continued.value(QStringLiteral("code")).toString()));
}

void H4OperationDiscovery::snapshotImmutabilityWhileBusinessAdvances() {
  QTemporaryDir root;
  Host host(root.path());
  const QString scope = "agent:h4:chat:imm";
  OperationLog log(logDir(root.path()), root.path(), host.lock->claim());
  OperationLog::Record first = seed("imm-1", scope);
  OperationLog::Record second = seed("imm-2", scope);
  QCOMPARE(log.acceptDurable(first).code, QLatin1String("created"));
  QCOMPARE(log.acceptDurable(second).code, QLatin1String("created"));
  first = log.recordById(first.operationId);
  second = log.recordById(second.operationId);
  QCOMPARE(begin(log, first), QLatin1String("ok"));
  QCOMPARE(begin(log, second), QLatin1String("ok"));
  OperationDiscovery service;
  const DiscoveryScan scan{QFileInfo(root.path()).canonicalFilePath(), logDir(root.path()), "doc", scope,
                           host.lock->claim()};
  // 页 1（小页，必有续页）后业务推进：另一条记录转 failed。
  auto page = service.query(scan, {{QStringLiteral("page_size"), 1}});
  QVERIFY(page.value(QStringLiteral("ok")).toBool());
  const auto digest = page.value(QStringLiteral("coverage")).toObject().value(QStringLiteral("journalDigest")).toString();
  const QString pageOne = operationIds(page).first();
  OperationLog::Record &mutated = pageOne == first.operationId ? second : first;
  mutated.execution = QStringLiteral("failed");
  mutated.code = QStringLiteral("failed");
  QCOMPARE(update(log, mutated), QLatin1String("ok"));
  // 同一游标续页：不可变快照——digest 不变、被改记录仍按快照呈现 running。
  const auto next = service.query(scan, {{QStringLiteral("cursor"), page.value(QStringLiteral("nextCursor")).toString()},
                                         {QStringLiteral("page_size"), 1}});
  QVERIFY(next.value(QStringLiteral("ok")).toBool());
  QCOMPARE(next.value(QStringLiteral("coverage")).toObject().value(QStringLiteral("journalDigest")).toString(), digest);
  QCOMPARE(next.value(QStringLiteral("operations")).toArray().at(0).toObject()
               .value(QStringLiteral("execution")).toString(), QStringLiteral("running"));
  // 新查询取得新快照（新 digest + 新状态）。
  const auto fresh = service.query(scan, {});
  QVERIFY(fresh.value(QStringLiteral("ok")).toBool());
  QVERIFY(fresh.value(QStringLiteral("coverage")).toObject().value(QStringLiteral("journalDigest")).toString() != digest);
  bool sawFailed = false;
  for (const auto &entry : fresh.value(QStringLiteral("operations")).toArray())
    if (entry.toObject().value(QStringLiteral("operationId")).toString() == mutated.operationId)
      sawFailed = entry.toObject().value(QStringLiteral("execution")).toString() == QLatin1String("failed");
  QVERIFY(sawFailed);
}

void H4OperationDiscovery::relationsClassifyDirectTargets() {
  QTemporaryDir root;
  Host host(root.path());
  const QString scope = "agent:h4:chat:rel";
  const auto calls = std::make_shared<int>(0);
  const auto source = host.recovery.accept(plan("source", countingWorker(calls), scope));
  QVERIFY2(source.ok, qPrintable(source.code));
  ExecuteResult result;
  QString error;
  QVERIFY(runToEnd(host, source.record, result, &error));
  // B：记录里带 sourceOperationId（accept 生成 frozenRequest.sourceOperationId）。
  const auto consumer = host.recovery.accept(plan("consumer", {}, scope, source.record.operationId));
  QVERIFY2(consumer.ok, qPrintable(consumer.code));
  // C：合成行——来源指向不存在目标 + 计划内步骤 + 父版本。
  QJsonObject frozen{{QStringLiteral("intent"), QStringLiteral("rel")},
                     {QStringLiteral("sourceOperationId"), QStringLiteral("op-") + uuid()}};
  QJsonObject artifacts{{QStringLiteral("childOperations"),
      QJsonArray{QJsonObject{{QStringLiteral("nodeId"), QStringLiteral("n1")},
                             {QStringLiteral("operationId"), QStringLiteral("op-") + uuid()}},
                  QJsonObject{{QStringLiteral("nodeId"), QStringLiteral("n2")},
                             {QStringLiteral("operationId"), QStringLiteral("not-an-id")}}}},
      {QStringLiteral("ports"), QJsonArray{QJsonObject{{QStringLiteral("portId"), QStringLiteral("OUTPUT")},
                                                       {QStringLiteral("sha256"), QString(64, 'f')},
                                                       {QStringLiteral("byteSize"), QStringLiteral("4")}}}}};
  QJsonObject stage{{QStringLiteral("parents"), QJsonArray{QStringLiteral("ver-1"), QStringLiteral("ver-2")}}};
  const auto rowC = syntheticRow("doc", scope, "synthetic", frozen, artifacts, stage);
  {
    QFile file(journalPath(root.path()));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto rows = QJsonDocument::fromJson(file.readAll()).array();
    file.close();
    QJsonArray edited = rows;
    edited.append(rowC);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(edited).toJson(QJsonDocument::Compact));
  }
  // 消费者视角：source_operation 解析为真实同 scope 记录。
  QJsonObject page;
  QVERIFY(listOnce(host.recovery, scope, {{QStringLiteral("view"), QStringLiteral("relations")},
                                          {QStringLiteral("anchor_operation_id"), consumer.record.operationId}},
                   page, &error));
  QVERIFY(page.value(QStringLiteral("ok")).toBool());
  auto relations = page.value(QStringLiteral("relations")).toArray();
  QCOMPARE(relations.size(), 1);
  QCOMPARE(relations.at(0).toObject().value(QStringLiteral("relation")).toString(), QStringLiteral("source_operation"));
  QCOMPARE(relations.at(0).toObject().value(QStringLiteral("target")).toObject()
               .value(QStringLiteral("operationId")).toString(), source.record.operationId);
  // 来源视角：消费者 + 无未解析来源。
  QVERIFY(listOnce(host.recovery, scope, {{QStringLiteral("view"), QStringLiteral("relations")},
                                          {QStringLiteral("anchor_operation_id"), source.record.operationId}},
                   page, &error));
  QVERIFY(page.value(QStringLiteral("ok")).toBool());
  relations = page.value(QStringLiteral("relations")).toArray();
  QCOMPARE(relations.size(), 1);
  QCOMPARE(relations.at(0).toObject().value(QStringLiteral("relation")).toString(), QStringLiteral("consumer"));
  // 合成行视角：未解析来源（不回显目标 ID）+ 计划步骤 + 父版本。
  QVERIFY(listOnce(host.recovery, scope, {{QStringLiteral("view"), QStringLiteral("relations")},
                                          {QStringLiteral("anchor_operation_id"),
                                           rowC.value(QStringLiteral("operationId")).toString()}},
                   page, &error));
  QVERIFY(page.value(QStringLiteral("ok")).toBool());
  relations = page.value(QStringLiteral("relations")).toArray();
  QCOMPARE(relations.size(), 4);
  QCOMPARE(relations.at(0).toObject().value(QStringLiteral("relation")).toString(), QStringLiteral("source_operation"));
  QCOMPARE(relations.at(0).toObject().value(QStringLiteral("target")).toObject()
               .value(QStringLiteral("status")).toString(), QStringLiteral("unresolved_in_snapshot"));
  const auto encoded = QString::fromUtf8(QJsonDocument(page).toJson(QJsonDocument::Compact));
  QVERIFY(!encoded.contains(frozen.value(QStringLiteral("sourceOperationId")).toString()));
  QCOMPARE(relations.at(1).toObject().value(QStringLiteral("relation")).toString(), QStringLiteral("plan_step"));
  QCOMPARE(relations.at(1).toObject().value(QStringLiteral("nodeId")).toString(), QStringLiteral("n1"));
  QCOMPARE(relations.at(1).toObject().value(QStringLiteral("access")).toString(),
           QStringLiteral("plan_step_not_independently_accessible"));
  QVERIFY(relations.at(1).toObject().value(QStringLiteral("operationId")).isString()); // 记录事实保留
  QVERIFY(!relations.at(2).toObject().value(QStringLiteral("operationId")).isString()); // 非规范 ID 不冒充
  QCOMPARE(relations.at(3).toObject().value(QStringLiteral("kind")).toString(), QStringLiteral("catalog_version"));
  // 锚点越权/不存在：不扩大边界，不泄漏。
  QVERIFY(listOnce(host.recovery, "agent:h4:chat:other",
                   {{QStringLiteral("view"), QStringLiteral("relations")},
                    {QStringLiteral("anchor_operation_id"), source.record.operationId}}, page, &error));
  QCOMPARE(page.value(QStringLiteral("code")).toString(), QStringLiteral("operation_not_found"));
  QVERIFY(listOnce(host.recovery, scope, {{QStringLiteral("view"), QStringLiteral("relations")},
                                          {QStringLiteral("anchor_operation_id"), QStringLiteral("op-") + uuid()}},
                   page, &error));
  QCOMPARE(page.value(QStringLiteral("code")).toString(), QStringLiteral("operation_not_found"));
}

void H4OperationDiscovery::cancelPreservesJournalAndBusinessState() {
  QTemporaryDir root;
  QDir().mkpath(logDir(root.path()));
  const QString scope = "agent:h4:chat:cancel";
  QJsonArray rows;
  for (int i = 0; i < 600; ++i)
    rows.append(syntheticRow("doc", scope, QStringLiteral("c-%1").arg(i, 4, 10, QChar('0'))));
  {
    QFile file(journalPath(root.path()));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(rows).toJson(QJsonDocument::Compact));
  }
  Host host(root.path());
  const auto before = fileSha(journalPath(root.path()));
  // 在途业务操作（queued）：发现取消不得写 cancelRequested 或扰动其状态。
  const auto business = host.recovery.accept(plan("business", countingWorker(std::make_shared<int>()), scope));
  QVERIFY2(business.ok, qPrintable(business.code));
  QJsonObject out;
  bool delivered = false;
  // taskAdded 同步取消：worker 首个取消检查点前即置位，确定性走 cancelled 分支。
  const QMetaObject::Connection added =
      QObject::connect(&host.tasks, &PaleoTaskService::taskAdded, [&](PaleoTask *task) { task->requestCancel(); });
  auto *task = host.recovery.listOperations(scope, {}, &host.recovery,
                                            [&](QJsonObject r) { out = r; delivered = true; });
  QObject::disconnect(added);
  QVERIFY(task);
  QEventLoop loop;
  QObject::connect(task, &PaleoTask::finished, &loop, &QEventLoop::quit);
  loop.exec();
  for (int i = 0; i < 500 && !delivered; ++i) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  QVERIFY(delivered);
  QCOMPARE(out.value(QStringLiteral("code")).toString(), QStringLiteral("cancelled"));
  QVERIFY(!out.value(QStringLiteral("ok")).toBool());
  // 零副作用：journal 字节不变，业务记录仍是 queued/未取消。
  QCOMPARE(fileSha(journalPath(root.path())), before);
  const auto state = OperationLog(logDir(root.path()), root.path(), host.lock->claim())
                         .recordById(business.record.operationId);
  QCOMPARE(state.execution, QStringLiteral("queued"));
  QVERIFY(!state.cancelRequested);
}

void H4OperationDiscovery::subscriberDestructionKeepsServiceUsable() {
  QTemporaryDir root;
  Host host(root.path());
  const QString scope = "agent:h4:chat:destroy";
  const auto accepted = host.recovery.accept(plan("op", countingWorker(std::make_shared<int>()), scope));
  QVERIFY2(accepted.ok, qPrintable(accepted.code));
  QJsonObject out;
  bool delivered = false;
  {
    auto *subscriber = new QObject();
    auto *task = host.recovery.listOperations(scope, {}, subscriber,
                                              [&](QJsonObject r) { out = r; delivered = true; });
    QVERIFY(task);
    delete subscriber; // 完成前销毁订阅者：迟到结果不投递、不悬空。
    QEventLoop loop;
    QObject::connect(task, &PaleoTask::finished, &loop, &QEventLoop::quit);
    loop.exec();
    for (int i = 0; i < 50; ++i)
      QCoreApplication::processEvents();
  }
  QVERIFY(!delivered);
  // 服务缓存/状态未被旧回调清掉：新查询照常工作。
  QJsonObject page;
  QString error;
  QVERIFY(listOnce(host.recovery, scope, {}, page, &error));
  QVERIFY(page.value(QStringLiteral("ok")).toBool());
  QCOMPARE(operationIds(page).size(), 1);
}

void H4OperationDiscovery::readGateAndWriteDenialBoundaries() {
  QTemporaryDir root;
  {
    Host host(root.path());
    host.allowedEffects = 0; // Read 也被拒：发现如实失败，不启动扫描。
    QJsonObject page;
    QString error;
    QVERIFY(!host.recovery.listOperations("agent:h4:chat:a", {}, &host.recovery,
                                          [&](QJsonObject) {}, &error));
    QCOMPARE(error, QStringLiteral("effect_denied"));
  }
  Host host(root.path());
  host.allowedEffects = host::Read; // 只读宿主：写权限不足不阻止只读发现。
  host.ownsWrites = false;
  const QString scope = "agent:h4:chat:ro";
  const auto accepted = host.recovery.accept(plan("ro", countingWorker(std::make_shared<int>()), scope));
  QVERIFY(!accepted.ok); // 写路径如实拒绝（效果门在声明层拦截）
  QCOMPARE(accepted.code, QStringLiteral("effect_denied"));
  QDir().mkpath(logDir(root.path()));
  {
    QFile file(journalPath(root.path()));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonArray{syntheticRow("doc", scope, "r1")}).toJson(QJsonDocument::Compact));
  }
  QJsonObject page;
  QString error;
  QVERIFY(listOnce(host.recovery, scope, {}, page, &error));
  QVERIFY(page.value(QStringLiteral("ok")).toBool());
  QCOMPARE(operationIds(page).size(), 1);
}

void H4OperationDiscovery::listDoesNotReconcileWhileGetDoes() {
  QTemporaryDir root;
  Host host(root.path());
  const QString scope = "agent:h4:chat:recon";
  OperationLog::Record record = seed("recon", scope);
  OperationLog log(logDir(root.path()), root.path(), host.lock->claim());
  QCOMPARE(log.acceptDurable(record).code, QLatin1String("created"));
  record = log.recordById(record.operationId);
  QCOMPARE(begin(log, record), QLatin1String("ok"));
  record.execution = QStringLiteral("prepared");
  record.code = QStringLiteral("prepared");
  record.preparedManifest = {{QStringLiteral("staged"), true}};
  record.commitState = QStringLiteral("publishing");
  QCOMPARE(update(log, record), QLatin1String("ok"));
  const auto before = fileSha(journalPath(root.path()));
  // 纯 list：零 reconcile 写——journal 字节与 publishing 状态原样。
  QJsonObject page;
  QString error;
  QVERIFY(listOnce(host.recovery, scope, {}, page, &error));
  QVERIFY(page.value(QStringLiteral("ok")).toBool());
  QCOMPARE(fileSha(journalPath(root.path())), before);
  QCOMPARE(page.value(QStringLiteral("operations")).toArray().at(0).toObject()
               .value(QStringLiteral("commitState")).toString(), QStringLiteral("publishing"));
  QCOMPARE(log.recordById(record.operationId).commitState, QStringLiteral("publishing"));
  // 显式 get：沿旧契约对账（publishing 无回执 → 回退 not_requested）。
  const auto state = host.recovery.get(scope, record.operationId);
  QVERIFY(state.value(QStringLiteral("ok")).toBool());
  QVERIFY(fileSha(journalPath(root.path())) != before);
  QCOMPARE(log.recordById(record.operationId).commitState, QStringLiteral("not_requested"));
}

QTEST_MAIN(H4OperationDiscovery)
#include "tst_h4_operation_discovery.moc"
