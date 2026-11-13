// 层：测试壳
// U1 CommandDispatcher：登记门、状态原因、触发、入口面（Agent 位），以及
// Compute 命令 → CapabilityGateway 的桥（同一输入与直连网关结果一致）。
#include <QtTest/QtTest>

#include <QSignalSpy>
#include <QTemporaryDir>

#include "workflow/commanddispatcher.h"
#include "workflow/depthconversionworkflow.h"

using namespace paleo::command;
using paleo::capability::ExecuteRequest;
using paleo::capability::ExecuteResult;

namespace {

CommandContext uiCtx() {
  CommandContext c;
  c.documentId = QStringLiteral("doc-1");
  c.callerScope = QStringLiteral("ui");
  return c;
}

CommandContext agentCtx(const QString &client = QStringLiteral("agent-req-1")) {
  CommandContext c = uiCtx();
  c.callerScope = QStringLiteral("agent:session-1");
  c.clientRequestId = client;
  return c;
}

paleo::velmodel::VelocityModel model() {
  paleo::velmodel::VelocityWellControl well;
  well.wellId = QStringLiteral("W1");
  well.x = 0;
  well.y = 0;
  well.knots = {{100, 200}, {200, 500}};
  QString error;
  return paleo::velmodel::VelocityModel::fit({well}, paleo::velmodel::ModelType::IntervalAverage, &error);
}

QVariantMap depthParams() {
  return {{QStringLiteral("TIME_SEMANTIC"), QStringLiteral("TWT")},
          {QStringLiteral("DEPTH_SEMANTIC"), QStringLiteral("TVD")},
          {QStringLiteral("EXTRAPOLATION"), QStringLiteral("reject_outside")},
          {QStringLiteral("MODEL_REVISION"), QStringLiteral("rev-1")}};
}

QVariantMap depthInputs() {
  QVariantMap sample{{QStringLiteral("sampleId"), QStringLiteral("s1")},
                     {QStringLiteral("wellId"), QStringLiteral("W1")},
                     {QStringLiteral("occurrenceId"), QStringLiteral("occ-1")},
                     {QStringLiteral("time"), 150.0}};
  return {{QStringLiteral("samples"), QVariantList{sample}},
          {QStringLiteral("model"), model().toJson().toVariantMap()}};
}

} // namespace

class TestCommandDispatcher : public QObject {
  Q_OBJECT
private slots:
  void bindRejectsUnregisteredIds() {
    CommandDispatcher d;
    QString error;
    QVERIFY(!d.bind(QStringLiteral("not.registered"), [](const CommandContext &, const QVariantMap &) {}, {},
                    &error));
    QVERIFY(error.contains(QStringLiteral("unregistered")));
    QVERIFY(!d.bind(QStringLiteral("project.open"), {}, {}, &error));
    QVERIFY(d.bind(QStringLiteral("project.open"), [](const CommandContext &, const QVariantMap &) {}));
    QVERIFY(d.isBound(QStringLiteral("project.open")));
    QCOMPARE(d.boundIds(), QStringList{QStringLiteral("project.open")});
    d.unbind(QStringLiteral("project.open"));
    QVERIFY(!d.isBound(QStringLiteral("project.open")));
  }

  void stateAlwaysCarriesAReasonWhenDisabled() {
    CommandDispatcher d;
    CommandState st = d.state(QStringLiteral("no.such"), uiCtx());
    QVERIFY(!st.enabled);
    QVERIFY(!st.reason.isEmpty());
    st = d.state(QStringLiteral("project.open"), uiCtx()); // 未接入
    QVERIFY(!st.enabled);
    QVERIFY(!st.reason.isEmpty());
    bool open = false;
    d.bind(QStringLiteral("project.open"), [](const CommandContext &, const QVariantMap &) {},
           [&open](const CommandContext &) {
             CommandState s;
             s.enabled = open;
             return s; // 故意不给原因：dispatcher 兜底补「当前不可用」
           });
    st = d.state(QStringLiteral("project.open"), uiCtx());
    QVERIFY(!st.enabled);
    QVERIFY(!st.reason.isEmpty());
    open = true;
    st = d.state(QStringLiteral("project.open"), uiCtx());
    QVERIFY(st.enabled);
    QVERIFY(st.reason.isEmpty());
  }

  void triggerRunsHandlerOnlyWhenEnabled() {
    CommandDispatcher d;
    int calls = 0;
    QVariantMap seen;
    bool open = false;
    d.bind(
        QStringLiteral("data.list.unresolved"),
        [&](const CommandContext &ctx, const QVariantMap &p) {
          ++calls;
          seen = p;
          QCOMPARE(ctx.callerScope, QStringLiteral("ui"));
        },
        [&open](const CommandContext &) {
          CommandState s;
          s.enabled = open;
          s.reason = QStringLiteral("先打开工程");
          return s;
        });
    QSignalSpy rejected(&d, &CommandDispatcher::commandRejected);
    QSignalSpy triggered(&d, &CommandDispatcher::commandTriggered);
    QString reason;
    QVERIFY(!d.trigger(QStringLiteral("data.list.unresolved"), uiCtx(), {}, &reason));
    QCOMPARE(reason, QStringLiteral("先打开工程"));
    QCOMPARE(calls, 0);
    QCOMPARE(rejected.size(), 1);
    open = true;
    QVERIFY(d.trigger(QStringLiteral("data.list.unresolved"), uiCtx(), {{QStringLiteral("checked"), true}}));
    QCOMPARE(calls, 1);
    QCOMPARE(seen.value(QStringLiteral("checked")).toBool(), true);
    QCOMPARE(triggered.size(), 1);
    QCOMPARE(triggered.first().at(1).toString(), QStringLiteral("ui"));
  }

  // Agent 只能调 surfaces 含 Agent 且 exposure == Callable 的命令；UI 命令一律拒。
  void agentSurfaceGate() {
    CommandDispatcher d;
    int calls = 0;
    d.bind(QStringLiteral("project.open"), [&](const CommandContext &, const QVariantMap &) { ++calls; });
    QString reason;
    QVERIFY(!d.trigger(QStringLiteral("project.open"), agentCtx(), {}, &reason));
    QVERIFY(!reason.isEmpty());
    QCOMPARE(calls, 0);
    QVERIFY(d.trigger(QStringLiteral("project.open"), uiCtx()));
    QCOMPARE(calls, 1);
    // 无 UI 处理器的 Compute 命令对 Agent 可用（走 prepare/execute 桥），对 UI 不可用。
    QVERIFY(d.state(QStringLiteral("depth.convert_batch"), agentCtx()).enabled);
    QVERIFY(!d.state(QStringLiteral("depth.convert_batch"), uiCtx()).enabled);
    // 预测有 ribbon 入口但能力 Unavailable：不开放给 Agent。
    QVERIFY(!d.state(QStringLiteral("predict.run"), agentCtx()).enabled);
  }

  void prepareMapsCommandOntoExecuteRequest() {
    CommandDispatcher d;
    d.setHostBinder([](ExecuteRequest &r) { r.operationDir = QStringLiteral("/ops"); });
    const PreparedPlan plan = d.prepare(QStringLiteral("depth.convert_batch"), agentCtx(), depthParams(),
                                        depthInputs(), QStringLiteral("compute-only"));
    QVERIFY2(plan.ok, qPrintable(plan.code));
    QCOMPARE(plan.request.capabilityId, QStringLiteral("depth.convert_batch"));
    QCOMPARE(plan.request.contractVersion, 1);
    QCOMPARE(plan.request.documentId, QStringLiteral("doc-1"));
    QCOMPARE(plan.request.callerScope, QStringLiteral("agent:session-1"));
    QCOMPARE(plan.request.clientRequestId, QStringLiteral("agent-req-1"));
    QCOMPARE(plan.request.publishIntent, QStringLiteral("compute-only"));
    QCOMPARE(plan.request.operationDir, QStringLiteral("/ops"));
    QCOMPARE(plan.request.parameters, depthParams()); // 原样交网关
    QVERIFY(plan.effectiveParameters.contains(QStringLiteral("TIME_SEMANTIC")));
    // Agent must retain the request key across reconnects; only UI may create a new one.
    CommandContext noClient = agentCtx(QString());
    QCOMPARE(d.prepare(QStringLiteral("depth.convert_batch"), noClient, depthParams(), depthInputs()).code,
             QStringLiteral("client_request_id_required"));
  }

  void prepareRejections() {
    CommandDispatcher d;
    QCOMPARE(d.prepare(QStringLiteral("no.such"), uiCtx(), {}).code, QStringLiteral("unknown_command"));
    QCOMPARE(d.prepare(QStringLiteral("project.open"), uiCtx(), {}).code, QStringLiteral("not_compute"));
    QCOMPARE(d.prepare(QStringLiteral("predict.run"), uiCtx(), {}).code, QStringLiteral("not_compute"));
    // 必填参数缺失由 AlgorithmSpec 校验（与网关同一口径）。
    QVariantMap bad = depthParams();
    bad.remove(QStringLiteral("EXTRAPOLATION"));
    const PreparedPlan plan = d.prepare(QStringLiteral("depth.convert_batch"), agentCtx(), bad, depthInputs());
    QVERIFY(!plan.ok);
    QVERIFY(!plan.code.isEmpty());
    // 未准备好的计划不执行。
    int executed = 0;
    d.setExecutor([&executed](const ExecuteRequest &) {
      ++executed;
      return ExecuteResult();
    });
    QVERIFY(!d.execute(plan).ok);
    QCOMPARE(executed, 0);
  }

  void executeUsesInjectedExecutorAndReportsOperation() {
    CommandDispatcher d;
    ExecuteRequest seen;
    d.setExecutor([&seen](const ExecuteRequest &r) {
      seen = r;
      ExecuteResult res;
      res.ok = true;
      res.operationId = QStringLiteral("op-42");
      return res;
    });
    QSignalSpy op(&d, &CommandDispatcher::operationChanged);
    const PreparedPlan plan = d.prepare(QStringLiteral("constraint.idw"), uiCtx(), {});
    QVERIFY2(plan.ok, qPrintable(plan.code));
    QVERIFY(d.execute(plan).ok);
    QCOMPARE(seen.capabilityId, QStringLiteral("constraint.idw"));
    QCOMPARE(op.size(), 1);
    QCOMPARE(op.first().at(0).toString(), QStringLiteral("op-42"));
    QCOMPARE(op.first().at(1).toString(), QStringLiteral("constraint.idw"));
  }

  // 真实网关：经 dispatcher 的结果与直连 CapabilityGateway 一致（ATOMIC §10「同一输入同一有效参数」）。
  void bridgeMatchesDirectGateway() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    CommandDispatcher d;
    d.setHostBinder([&dir](ExecuteRequest &r) {
      r.operationDir = dir.path() + QStringLiteral("/via-dispatcher");
      r.bindingMode = QStringLiteral("pinned-value");
      r.frozenHead = QStringLiteral("h1");
      r.currentHead = QStringLiteral("h1");
    });
    const PreparedPlan plan = d.prepare(QStringLiteral("depth.convert_batch"), agentCtx(QStringLiteral("via")),
                                        depthParams(), depthInputs(), QStringLiteral("compute-only"));
    QVERIFY2(plan.ok, qPrintable(plan.code));
    QSignalSpy op(&d, &CommandDispatcher::operationChanged);
    const ExecuteResult viaDispatcher = d.execute(plan);
    QVERIFY2(viaDispatcher.ok, qPrintable(viaDispatcher.code));
    QCOMPARE(op.size(), 1);

    ExecuteRequest direct = plan.request;
    direct.operationDir = dir.path() + QStringLiteral("/direct");
    direct.clientRequestId = QStringLiteral("direct");
    const ExecuteResult viaGateway = paleo::capability::execute(direct);
    QVERIFY2(viaGateway.ok, qPrintable(viaGateway.code));
    const auto depth = [](const ExecuteResult &r) {
      return r.summary.value(QStringLiteral("samples")).toList().first().toMap().value(QStringLiteral("depthM"));
    };
    QVERIFY(depth(viaDispatcher).isValid());
    QCOMPARE(depth(viaDispatcher), depth(viaGateway));
    QCOMPARE(viaDispatcher.requestHash, viaGateway.requestHash);

    // 网关自身的身份门照旧生效：宿主没给 operationDir → 网关拒绝（dispatcher 不绕过）。
    CommandDispatcher bare;
    const ExecuteResult noDir =
        bare.execute(bare.prepare(QStringLiteral("depth.convert_batch"), agentCtx(QStringLiteral("bare")),
                                  depthParams(), depthInputs(), QStringLiteral("compute-only")));
    QVERIFY(!noDir.ok);
    QCOMPARE(noDir.code, QStringLiteral("operation_identity_required"));
  }

  void contextProvider() {
    CommandDispatcher d;
    QCOMPARE(d.currentContext().callerScope, QStringLiteral("ui"));
    d.setContextProvider([] {
      CommandContext c;
      c.horizon = QStringLiteral("C6");
      return c;
    });
    QCOMPARE(d.currentContext().horizon, QStringLiteral("C6"));
  }
};

QTEST_GUILESS_MAIN(TestCommandDispatcher)
#include "tst_commanddispatcher.moc"
