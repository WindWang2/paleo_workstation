// 层：测试壳
#include <QtTest>
#include <QFileInfo>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <memory>

#include "../src/catalog/datacatalog.h"
#include "../src/workflow/aichattoolrunner.h"
#include "../src/workflow/capabilitytaskrunner.h"

namespace {

ChatToolCall prepareCall(const QString &id) {
  const QJsonObject arguments{
      {QStringLiteral("client_request_id"), id},
      {QStringLiteral("binding_mode"), QStringLiteral("pinned-value")},
      {QStringLiteral("publish_intent"), QStringLiteral("prepare")},
      {QStringLiteral("parameters"), QJsonObject{{QStringLiteral("FIELD"), QStringLiteral("z")},
                                                 {QStringLiteral("CELL_SIZE"), 10.0}}},
      {QStringLiteral("inputs"), QJsonObject{{QStringLiteral("points_ref"), QStringLiteral("ver-points")},
                                             {QStringLiteral("subinterval_id"), QStringLiteral("sub-1")}}}};
  return {id, QStringLiteral("paleo.constraint_idw"),
          QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact))};
}

AiChatToolContext contextFor(const QTemporaryDir &directory, const QString &documentId) {
  AiChatToolContext context;
  context.projectDir = directory.path();
  context.documentId = documentId;
  return context;
}

class WorkerGate {
public:
  explicit WorkerGate(PaleoTaskService &tasks) {
    tasks.setMaxWorkerThreads(1);
    tasks.start(QStringLiteral("blocker"), [gate = m_gate](PaleoTask *) {
      gate->acquire();
      return QString();
    });
  }
  ~WorkerGate() { release(); }
  void release() {
    if (m_gate) {
      m_gate->release();
      m_gate.reset();
    }
  }

private:
  std::shared_ptr<QSemaphore> m_gate = std::make_shared<QSemaphore>();
};

} // namespace

class TestAiChatCapability : public QObject {
  Q_OBJECT

private slots:
  void capabilityCallsAreAsynchronousAndFifo() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    PaleoTaskService tasks;
    WorkerGate gate(tasks);
    AiChatToolRunner runner;
    runner.setTaskService(&tasks);
    runner.setContext(contextFor(project, QStringLiteral("document")));

    QStringList events;
    QVector<QJsonObject> results;
    connect(&runner, &AiChatToolRunner::toolStarted, this, [&](const ChatToolCall &call) {
      events.append(QStringLiteral("start ") + call.id);
    });
    connect(&runner, &AiChatToolRunner::toolFinished, this,
            [&](const ChatToolCall &call, bool, const QString &json) {
      events.append(QStringLiteral("finish ") + call.id);
      results.append(QJsonDocument::fromJson(json.toUtf8()).object());
    });
    runner.run(prepareCall(QStringLiteral("first")));
    runner.run(prepareCall(QStringLiteral("second")));
    QCOMPARE(tasks.tasks().size(), 2); // One blocker and only the first chat operation.
    QCoreApplication::processEvents();
    QVERIFY(results.isEmpty());

    gate.release();
    QTRY_COMPARE(results.size(), 2);
    QCOMPARE(events, (QStringList{QStringLiteral("start first"), QStringLiteral("finish first"),
                                  QStringLiteral("start second"), QStringLiteral("finish second")}));
    for (const auto &result : results) {
      QCOMPARE(result.value(QStringLiteral("code")).toString(), QStringLiteral("prepared"));
      QCOMPARE(result.value(QStringLiteral("commitState")).toString(), QStringLiteral("not_requested"));
      QVERIFY(!result.value(QStringLiteral("operationId")).toString().isEmpty());
    }
  }

  void projectChangeCannotRunOldQueuedCallsInNewProject() {
    QTemporaryDir oldProject, newProject;
    QVERIFY(oldProject.isValid() && newProject.isValid());
    PaleoTaskService tasks;
    WorkerGate gate(tasks);
    AiChatToolRunner runner;
    runner.setTaskService(&tasks);
    runner.setContext(contextFor(oldProject, QStringLiteral("old")));
    QStringList codes;
    connect(&runner, &AiChatToolRunner::toolFinished, this,
            [&](const ChatToolCall &, bool, const QString &json) {
      codes.append(QJsonDocument::fromJson(json.toUtf8()).object().value(QStringLiteral("code")).toString());
    });
    runner.run(prepareCall(QStringLiteral("active-old")));
    runner.run(prepareCall(QStringLiteral("queued-old")));
    tasks.beginNewSession();
    runner.setContext(contextFor(newProject, QStringLiteral("new")));
    runner.run(prepareCall(QStringLiteral("new")));
    gate.release();

    QTRY_COMPARE(codes.size(), 3);
    QCOMPARE(codes, (QStringList{QStringLiteral("stale_session"), QStringLiteral("stale_session"),
                                 QStringLiteral("prepared")}));
    QVERIFY(!QFileInfo::exists(oldProject.filePath(QStringLiteral(".paleo/capability-ops/operations.json"))));
    QVERIFY(QFileInfo::exists(newProject.filePath(QStringLiteral(".paleo/capability-ops/operations.json"))));
  }

  void contextChangeDuringTaskAddedCancelsTheTask() {
    QTemporaryDir oldProject, newProject;
    QVERIFY(oldProject.isValid() && newProject.isValid());
    PaleoTaskService tasks;
    WorkerGate gate(tasks);
    AiChatToolRunner runner;
    runner.setTaskService(&tasks);
    runner.setContext(contextFor(oldProject, QStringLiteral("old")));
    QSignalSpy finished(&runner, &AiChatToolRunner::toolFinished);
    PaleoTask *task = nullptr;
    connect(&tasks, &PaleoTaskService::taskAdded, this, [&](PaleoTask *added) {
      task = added;
      runner.setContext(contextFor(newProject, QStringLiteral("new")));
    });
    runner.run(prepareCall(QStringLiteral("old")));
    QVERIFY(task);
    QVERIFY(task->cancelRequested());
    gate.release();

    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(!finished.front().at(1).toBool());
    const auto response = QJsonDocument::fromJson(finished.front().at(2).toString().toUtf8()).object();
    QCOMPARE(response.value(QStringLiteral("code")).toString(), QStringLiteral("stale_session"));
    QVERIFY(!runner.busy());
    QVERIFY(!QFileInfo::exists(oldProject.filePath(QStringLiteral(".paleo/capability-ops/operations.json"))));
  }

  void rejectedRequestsDoNotStartTasks() {
    using namespace paleo::capability;
    QTemporaryDir project;
    QVERIFY(project.isValid());
    PaleoTaskService tasks;
    AiChatToolRunner runner;
    runner.setTaskService(&tasks);
    runner.setContext(contextFor(project, QStringLiteral("document")));

    ExecuteRequest publish;
    publish.capabilityId = QStringLiteral("constraint.idw");
    publish.publishIntent = QStringLiteral("publish");
    QString error;
    QVERIFY(!startCapabilityTask(&tasks, this, QStringLiteral("publish"), publish, {}, &error));
    QCOMPARE(error, QStringLiteral("publish_not_allowed"));
    publish.capabilityId = QStringLiteral("depth.convert_batch");
    publish.publishIntent = QStringLiteral("prepare");
    QVERIFY(!startCapabilityTask(&tasks, this, QStringLiteral("prepare"), publish, {}, &error));
    QCOMPARE(error, QStringLiteral("prepare_not_supported"));

    QJsonObject result;
    connect(&runner, &AiChatToolRunner::toolFinished, this,
            [&](const ChatToolCall &, bool, const QString &json) {
      result = QJsonDocument::fromJson(json.toUtf8()).object();
    });
    auto oversized = prepareCall(QStringLiteral("oversized"));
    oversized.argumentsJson = QStringLiteral("{\"padding\":\"") + QString(24 * 1024, QChar(0x4E00))
        + QStringLiteral("\"}");
    QVERIFY(!oversized.argumentsWithinBudget());
    QVERIFY(!oversized.hasValidArguments());
    runner.run(oversized);
    QTRY_VERIFY(!result.isEmpty());
    QCOMPARE(result.value(QStringLiteral("code")).toString(), QStringLiteral("request_budget_exceeded"));
    QVERIFY(tasks.tasks().isEmpty());
  }

  void readOnlyProjectDoesNotWriteReceipts() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    DataCatalog catalog;
    QVERIFY(catalog.open(project.path()));
    catalog.setLockedReadOnly(true);
    PaleoTaskService tasks;
    AiChatToolRunner runner;
    runner.setTaskService(&tasks);
    auto context = contextFor(project, QStringLiteral("document"));
    context.catalog = &catalog;
    runner.setContext(context);
    QSignalSpy finished(&runner, &AiChatToolRunner::toolFinished);
    runner.run(prepareCall(QStringLiteral("read-only")));
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(!finished.front().at(1).toBool());
    const auto response = QJsonDocument::fromJson(finished.front().at(2).toString().toUtf8()).object();
    QCOMPARE(response.value(QStringLiteral("code")).toString(), QStringLiteral("project_read_only"));
    QVERIFY(tasks.tasks().isEmpty());
    QVERIFY(!QFileInfo::exists(project.filePath(QStringLiteral(".paleo/capability-ops"))));
  }

  void lateCancellationPreservesCompletedOperation_data() {
    QTest::addColumn<bool>("expectedSuccess");
    QTest::newRow("prepared") << true;
    QTest::newRow("persisted-failure") << false;
  }

  void lateCancellationPreservesCompletedOperation() {
    using namespace paleo::capability;
    QFETCH(bool, expectedSuccess);
    QTemporaryDir project;
    QVERIFY(project.isValid());
    PaleoTaskService tasks;
    const auto call = prepareCall(QStringLiteral("late"));
    ExecuteRequest request;
    const auto arguments = QJsonDocument::fromJson(call.argumentsJson.toUtf8()).object();
    QVERIFY(prepareChatToolRequest(request, call.name, arguments, QStringLiteral("test"),
                                   QStringLiteral("document"), project.path()).isEmpty());
    if (!expectedSuccess)
      request.inputs.remove(QStringLiteral("subinterval_id"));
    ExecuteResult result;
    bool completed = false;
    auto *task = startCapabilityTask(&tasks, this, QStringLiteral("prepare"), request,
        [&](ExecuteResult response) {
          result = std::move(response);
          completed = true;
        });
    QVERIFY(task);
    // The worker has finished. Its queued owner callback has not run yet.
    QVERIFY(tasks.workerPool()->waitForDone(5000));
    QVERIFY(!completed);
    task->requestCancel();
    QTRY_VERIFY(completed);
    QCOMPARE(task->state(), PaleoTask::State::Cancelled);
    QCOMPARE(result.ok, expectedSuccess);
    QCOMPARE(result.code, expectedSuccess ? QStringLiteral("prepared") : QStringLiteral("missing_required"));
    QCOMPARE(result.commitState, QStringLiteral("not_requested"));
    QVERIFY(!result.operationId.isEmpty());
    const auto receipt = OperationLog(project.path()).recordById(result.operationId);
    QCOMPARE(receipt.code, result.code);
    QCOMPARE(receipt.execution, result.execution);
  }
};

QTEST_GUILESS_MAIN(TestAiChatCapability)
#include "tst_aichatcapability.moc"
