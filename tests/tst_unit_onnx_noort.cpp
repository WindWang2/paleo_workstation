// 层：测试壳
// I2 无 ORT 构建档（PALEO_ONNXRUNTIME=OFF）：unit.onnx_predict 在编译期没有
// ONNX Runtime 时的结构化不可用——不是 QSKIP，也不冒充可用。
// · 委托函数与有宿主的 readiness 路径：只有需要新加载/数值运行的 prepare 与
//   新 compute 在调度前即 ort_unavailable；publish、终态重放与列表广播不受
//   牵连（已认证输出的发布不需要当前 ORT）。
// · 工具表仍登记该工具（发布/发现可用），静态路由存在——执行面负责如实报错，
//   不是「未接线」。
// 入口函数（prepareUnitOnnxPlan / computeUnitOnnxPlan）的 ort_unavailable 断言
// 在 tst_unit_onnx_operation 的无 ORT 分支。
#include <QtTest>
#include "ai/chat/domaintools.h"
#include "ai/chat/unitonnxtools.h"
#include "services/paleotaskservice.h"
#include "services/unitonnxcapabilityspec.h"
#include "workflow/aichattoolrunner.h"
#include "workflow/capabilityhost.h"

#if PALEO_HAVE_ORT
#error "tst_unit_onnx_noort 只在 PALEO_ONNXRUNTIME=OFF 构建登记；ORT 构建档见 tst_unit_onnx_agent"
#endif

namespace {
using paleo::host::Read;
using paleo::host::PrivateStage;
// 手工宿主绑定：readiness 只读 CallContext 的值（不触 catalog/任务调度）。
paleo::capability::HostBinding openProjectBinding()
{
  paleo::capability::HostBinding binding;
  binding.context.open = true;
  binding.context.documentId = QStringLiteral("noort-document");
  binding.context.persistentIdentity = true;
  binding.context.readOnly = false;
  binding.context.ownsWrites = true;
  binding.context.allowedEffects = Read | PrivateStage | paleo::host::CatalogPublish | paleo::host::Presentation;
  binding.projectDir = QStringLiteral("/nonexistent/noort-project");
  return binding;
}
} // namespace

class TestUnitOnnxNoOrt : public QObject
{
  Q_OBJECT
private slots:
  void readinessDelegateReportsStructuredUnavailable()
  {
    const QJsonObject prepare{{"publish_intent", "prepare"}};
    const QJsonObject compute{{"publish_intent", "compute-only"}};
    const QJsonObject publish{{"publish_intent", "publish"}};
    QCOMPARE(unitOnnxReadinessCode(nullptr, prepare, false), QStringLiteral("ort_unavailable"));
    QCOMPARE(unitOnnxReadinessCode(nullptr, compute, false), QStringLiteral("ort_unavailable"));
    QVERIFY(unitOnnxReadinessCode(nullptr, publish, false).isEmpty());
    QVERIFY(unitOnnxReadinessCode(nullptr, {}, true).isEmpty());
    QVERIFY(unitOnnxReadinessCode(nullptr, prepare, true).isEmpty());  // 列表广播不设门
  }

  void hostedReadinessBlocksOnlyPhasesThatNeedNewInference()
  {
    PaleoTaskService tasks;
    paleo::capability::CapabilityHost host;
    host.setTaskService(&tasks);
    host.setBindingProvider([] { return openProjectBinding(); });
    AiChatToolRunner runner;
    runner.setTaskService(&tasks);
    AiChatToolContext context;
    context.capabilityHost = &host;
    context.requireCapabilityHost = true;
    context.documentId = host.context().documentId;
    context.documentEpoch = host.context().epoch;
    context.projectDir = QStringLiteral("/nonexistent/noort-project");
    runner.setContext(context);
    const QString tool(QStringLiteral("paleo.unit_onnx_predict"));
    // 需要新加载/数值运行的阶段：调度前即拒，不建任务。
    const auto prepare = runner.readiness(tool, {{"publish_intent", "prepare"}}, false);
    QVERIFY(!prepare.ready);
    QCOMPARE(prepare.code, QStringLiteral("ort_unavailable"));
    const auto compute = runner.readiness(tool, {{"publish_intent", "compute-only"}}, false);
    QVERIFY(!compute.ready);
    QCOMPARE(compute.code, QStringLiteral("ort_unavailable"));
    QVERIFY(tasks.tasks().isEmpty());
    // 发布已认证输出与列表广播不受 ORT 缺席牵连。
    QVERIFY(runner.readiness(tool, {{"publish_intent", "publish"}}, false).ready);
    QVERIFY(runner.readiness(tool, {}, true).ready);
    // 工具仍在广播清单里（不是因缺 ORT 整体下架）。
    QStringList advertised;
    for (const auto &spec : runner.availableTools())
      advertised.append(spec.name);
    QCOMPARE(advertised.count(tool), 1);
  }

  void toolTableKeepsTheStaticRouteForPublication()
  {
    const AiToolSpec spec = aiToolSpec(QStringLiteral("paleo.unit_onnx_predict"));
    QVERIFY(!spec.name.isEmpty());
    QVERIFY(!spec.requiresLocalOrt);  // ORT 门按阶段给，不整工具下架
    QVERIFY(spec.validateParameters(QJsonObject{{"client_request_id", "k"},
                                                {"publish_intent", "publish"},
                                                {"inputs", QJsonObject{{"source_operation_id", "op-1"}}}})
                .isEmpty());
    QCOMPARE(dispatchAiTool(spec.name, QJsonObject{{"client_request_id", "k"},
                                                   {"publish_intent", "publish"},
                                                   {"inputs", QJsonObject{{"source_operation_id", "op-1"}}}})
                 .status,
             AiToolDispatchStatus::Routed);
    // 能力目录条目不依赖运行库。
    QCOMPARE(CapabilityRegistry::spec(QStringLiteral("unit.onnx_predict"), 1).exposure,
             CapabilityExposure::Callable);
  }
};

QTEST_GUILESS_MAIN(TestUnitOnnxNoOrt)
#include "tst_unit_onnx_noort.moc"
