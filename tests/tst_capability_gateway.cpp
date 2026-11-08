// 层：测试壳
#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#include "ai/chat/domaintools.h"

#include "algorithms/seisattrgroup.h"
#include "catalog/datacatalog.h"
#include "domain/stratigraphicunit.h"
#include "services/stratigraphicsubintervals.h"
#include "workflow/capabilitygateway.h"
#include "workflow/depthconversionworkflow.h"

namespace {

using paleo::capability::ExecuteRequest;
using paleo::capability::ExecuteResult;
using paleo::capability::OperationLog;
using paleo::capability::PlanEdge;
using paleo::capability::PlanNode;
using paleo::capability::ProcessBudgetLedger;

struct LimitGuard {
  qint64 previous;
  explicit LimitGuard(qint64 next) : previous(ProcessBudgetLedger::instance().limit()) {
    ProcessBudgetLedger::instance().setLimit(next);
  }
  ~LimitGuard() { ProcessBudgetLedger::instance().setLimit(previous); }
};

struct OperationWriteFault {
  explicit OperationWriteFault(int succeedBeforeFail) {
    OperationLog::armOperationFileWriteFault(succeedBeforeFail);
  }
  ~OperationWriteFault() { OperationLog::armOperationFileWriteFault(-1); }
};

paleo::velmodel::VelocityModel model() {
  paleo::velmodel::VelocityWellControl well;
  well.wellId = QStringLiteral("W1");
  well.x = 0;
  well.y = 0;
  well.knots = {{100, 200}, {200, 500}};
  QString error;
  return paleo::velmodel::VelocityModel::fit({well}, paleo::velmodel::ModelType::IntervalAverage, &error);
}

ExecuteRequest depthRequest(const QString &dir, const QString &client) {
  ExecuteRequest request;
  request.capabilityId = QStringLiteral("depth.convert_batch");
  request.contractVersion = 1;
  request.documentId = QStringLiteral("doc-1");
  request.callerScope = QStringLiteral("host-test");
  request.clientRequestId = client;
  request.bindingMode = QStringLiteral("pinned-value");
  request.frozenHead = QStringLiteral("h1");
  request.currentHead = QStringLiteral("h1");
  request.publishIntent = QStringLiteral("compute-only");
  request.operationDir = dir;
  request.parameters.insert(QStringLiteral("TIME_SEMANTIC"), QStringLiteral("TWT"));
  request.parameters.insert(QStringLiteral("DEPTH_SEMANTIC"), QStringLiteral("TVD"));
  request.parameters.insert(QStringLiteral("EXTRAPOLATION"), QStringLiteral("reject_outside"));
  request.parameters.insert(QStringLiteral("MODEL_REVISION"), QStringLiteral("rev-1"));
  QVariantMap sample;
  sample.insert(QStringLiteral("sampleId"), QStringLiteral("s1"));
  sample.insert(QStringLiteral("wellId"), QStringLiteral("W1"));
  sample.insert(QStringLiteral("occurrenceId"), QStringLiteral("occ-1"));
  sample.insert(QStringLiteral("time"), 150.0);
  request.inputs.insert(QStringLiteral("samples"), QVariantList{sample});
  request.inputs.insert(QStringLiteral("model"), model().toJson().toVariantMap());
  request.inputs.insert(QStringLiteral("path"), QStringLiteral("/tmp/ignored"));
  return request;
}

} // namespace

class TestCapabilityGateway : public QObject {
  Q_OBJECT
private slots:
  void threeDepthEntriesMatch();
  void replayConflictAndRestart();
  void cancelPublishAndPresentation();
  void seismicGroupBudgetAndCache();
  void envelopeBatchDoesNotRejectCoherence();
  void constraintPlanAndBarriers();
  void intervalsStaySplit();
  void toolProtocol();
  void planAndBudgetClaims();
  void reviewFixes();
  void secondReviewFixes();
  void splitCoherenceFileMatchesTiledInterior();
  void chatHostCatalogPublishes();
};

void TestCapabilityGateway::threeDepthEntriesMatch() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const auto oracle = model();
  const double expected = oracle.wellDepthForTwt(oracle.wells().first(), 150);
  ExecuteRequest batch = depthRequest(dir.path(), QStringLiteral("c-batch"));
  const ExecuteResult direct = paleo::capability::execute(batch);
  QVERIFY2(direct.ok, qPrintable(direct.code));
  QCOMPARE(direct.summary.value(QStringLiteral("samples")).toList().first().toMap().value(QStringLiteral("depthM")).toDouble(),
           expected);

  QJsonObject arguments;
  arguments.insert(QStringLiteral("contract_version"), 1);
  arguments.insert(QStringLiteral("client_request_id"), QStringLiteral("c-tool"));
  arguments.insert(QStringLiteral("binding_mode"), QStringLiteral("pinned-value"));
  arguments.insert(QStringLiteral("frozen_head"), QStringLiteral("h1"));
  arguments.insert(QStringLiteral("current_head"), QStringLiteral("h1"));
  arguments.insert(QStringLiteral("publish_intent"), QStringLiteral("compute-only"));
  arguments.insert(QStringLiteral("parameters"), QJsonObject::fromVariantMap(batch.parameters));
  arguments.insert(QStringLiteral("inputs"), QJsonObject::fromVariantMap(batch.inputs));
  const QJsonObject tool = paleo::capability::runChatTool(QStringLiteral("paleo.depth_convert_batch"), arguments,
                                                         QStringLiteral("host-test"), QStringLiteral("doc-1"), dir.path());
  QCOMPARE(tool.value(QStringLiteral("samples")).toArray().first().toObject().value(QStringLiteral("depthM")).toDouble(),
           expected);

  DepthConversionWorkflow workflow(nullptr, dir.path());
  QString code;
  QVariantMap parameters = batch.parameters;
  parameters.insert(QStringLiteral("client_request_id"), QStringLiteral("c-workflow"));
  const QVariantMap viaWorkflow = workflow.convertWellSamples(parameters, batch.inputs, dir.path(), &code);
  QCOMPARE(code, QStringLiteral("converted"));
  QCOMPARE(viaWorkflow.value(QStringLiteral("samples")).toList().first().toMap().value(QStringLiteral("depthM")).toDouble(),
           expected);
  QCOMPARE(direct.summary.value(QStringLiteral("extrapolation")).toString(),
           viaWorkflow.value(QStringLiteral("extrapolation")).toString());
}

void TestCapabilityGateway::replayConflictAndRestart() {
  QTemporaryDir dir;
  ExecuteRequest request = depthRequest(dir.path(), QStringLiteral("same"));
  const ExecuteResult first = paleo::capability::execute(request);
  QVERIFY(first.ok);
  const ExecuteResult replay = paleo::capability::execute(request);
  QCOMPARE(replay.code, QStringLiteral("replay"));
  QCOMPARE(replay.operationId, first.operationId);
  QCOMPARE(replay.summary.value(QStringLiteral("computeCount")).toInt(),
           first.summary.value(QStringLiteral("computeCount")).toInt());
  request.inputs.insert(QStringLiteral("samples"), QVariantList{});
  const ExecuteResult conflict = paleo::capability::execute(request);
  QCOMPARE(conflict.code, QStringLiteral("conflict"));
  OperationLog reopened(dir.path());
  const auto stored = reopened.recordById(first.operationId);
  QCOMPARE(stored.requestHash, first.requestHash);
  QCOMPARE(stored.execution, QStringLiteral("succeeded"));
}

void TestCapabilityGateway::cancelPublishAndPresentation() {
  QTemporaryDir dir;
  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  ExecuteRequest request = depthRequest(dir.path() + QStringLiteral("/ops"), QStringLiteral("cancel"));
  request.cancelled = [] { return true; };
  const ExecuteResult cancelled = paleo::capability::execute(request);
  QCOMPARE(cancelled.execution, QStringLiteral("cancelled"));
  QCOMPARE(catalog.versions().size(), 0);

  request.clientRequestId = QStringLiteral("publish");
  request.cancelled = {};
  request.publishIntent = QStringLiteral("publish");
  request.catalog = &catalog;
  request.projectDir = dir.path();
  const int before = catalog.versions().size();
  const ExecuteResult published = paleo::capability::execute(request);
  QVERIFY2(published.ok, qPrintable(published.code));
  QCOMPARE(published.commitState, QStringLiteral("committed"));
  QVERIFY(catalog.versions().size() == before + 1);
  const ExecuteResult again = paleo::capability::execute(request);
  QCOMPARE(again.code, QStringLiteral("replay"));
  QCOMPARE(catalog.versions().size(), before + 1);
  OperationLog log(request.operationDir);
  QCOMPARE(log.cancel(published.operationId), QStringLiteral("cancel_after_commit"));
  QCOMPARE(catalog.versions().size(), before + 1);

  request.clientRequestId = QStringLiteral("stale");
  request.currentHead = QStringLiteral("h2");
  const int atStale = catalog.versions().size();
  const ExecuteResult stale = paleo::capability::execute(request);
  QCOMPARE(stale.code, QStringLiteral("stale_revision"));
  QCOMPARE(catalog.versions().size(), atStale);

  request.clientRequestId = QStringLiteral("present");
  request.currentHead = QStringLiteral("h1");
  request.presentOk = false;
  const ExecuteResult pending = paleo::capability::execute(request);
  QCOMPARE(pending.code, QStringLiteral("presentation_pending"));
  QCOMPARE(pending.commitState, QStringLiteral("committed"));
  QCOMPARE(catalog.versions().size(), atStale + 1);

  request.clientRequestId = QStringLiteral("cycle");
  request.capabilityId = QStringLiteral("seismic.attributes.compute");
  request.parameters.clear();
  request.parameters.insert(QStringLiteral("ATTRIBUTES"), QStringList{QStringLiteral("rms"), QStringLiteral("envelope")});
  request.parameters.insert(QStringLiteral("SAMPLE_INTERVAL_MS"), 4.0);
  QVariantList trace;
  for (int i = 0; i < 8; ++i)
    trace.append(std::sin(0.3 * i));
  request.inputs.clear();
  request.inputs.insert(QStringLiteral("traces"), QVariantList{trace});
  request.inputs.insert(QStringLiteral("full_trace"), true);
  QVariantMap parents;
  parents.insert(QStringLiteral("RMS"), QStringList{QStringLiteral("ENVELOPE")});
  parents.insert(QStringLiteral("ENVELOPE"), QStringList{QStringLiteral("RMS")});
  request.inputs.insert(QStringLiteral("parents"), parents);
  request.presentOk = true;
  const int beforeCycle = catalog.versions().size();
  const ExecuteResult cycle = paleo::capability::execute(request);
  QVERIFY(!cycle.ok);
  QCOMPARE(catalog.versions().size(), beforeCycle);

  request.clientRequestId = QStringLiteral("drop");
  request.inputs.remove(QStringLiteral("parents"));
  request.inputs.insert(QStringLiteral("drop_output_port"), QStringLiteral("ENVELOPE"));
  const ExecuteResult dropped = paleo::capability::execute(request);
  QVERIFY(!dropped.ok);
  QCOMPARE(catalog.versions().size(), beforeCycle);
}

void TestCapabilityGateway::seismicGroupBudgetAndCache() {
  QTemporaryDir dir;
  ExecuteRequest request;
  request.capabilityId = QStringLiteral("seismic.attributes.compute");
  request.contractVersion = 1;
  request.documentId = QStringLiteral("doc-s");
  request.callerScope = QStringLiteral("host-test");
  request.clientRequestId = QStringLiteral("s1");
  request.bindingMode = QStringLiteral("pinned-value");
  request.frozenHead = QStringLiteral("h1");
  request.currentHead = QStringLiteral("h1");
  request.publishIntent = QStringLiteral("compute-only");
  request.operationDir = dir.path();
  request.parameters.insert(QStringLiteral("ATTRIBUTES"),
                            QStringList{QStringLiteral("rms"), QStringLiteral("envelope"), QStringLiteral("phase")});
  request.parameters.insert(QStringLiteral("SAMPLE_INTERVAL_MS"), 4.0);
  QVariantList trace;
  std::vector<float> raw(16);
  for (int i = 0; i < 16; ++i) {
    raw[static_cast<std::size_t>(i)] = std::sin(0.25 * i);
    trace.append(static_cast<double>(raw[static_cast<std::size_t>(i)]));
  }
  request.inputs.insert(QStringLiteral("traces"), QVariantList{trace});
  const ExecuteResult first = paleo::capability::execute(request);
  QVERIFY2(first.ok, qPrintable(first.code));
  QCOMPARE(first.summary.value(QStringLiteral("analyticEvaluations")).toInt(), 1);
  QCOMPARE(first.summary.value(QStringLiteral("samplesRead")).toInt(), 16);
  paleo::seisattr::TraceGroupRequest kernel;
  kernel.traces = raw.data();
  kernel.nTraces = 1;
  kernel.traceLength = 16;
  kernel.stride = 16;
  kernel.sampleIntervalMs = 4;
  kernel.wantEnvelope = true;
  kernel.wantPhase = true;
  kernel.wantRms = true;
  kernel.rmsHalf = 8;
  const auto grouped = paleo::seisattr::computeAttributeGroup(kernel);
  QCOMPARE(grouped.analyticEvaluations, first.summary.value(QStringLiteral("analyticEvaluations")).toInt());

  request.clientRequestId = QStringLiteral("s2");
  const ExecuteResult cached = paleo::capability::execute(request);
  QVERIFY(cached.cacheHit);
  QCOMPARE(cached.summary.value(QStringLiteral("analyticEvaluationsAdded")).toInt(), 0);
  QCOMPARE(cached.summary.value(QStringLiteral("envelopeSha256")).toString(),
           first.summary.value(QStringLiteral("envelopeSha256")).toString());

  QJsonObject arguments;
  arguments.insert(QStringLiteral("client_request_id"), QStringLiteral("s-tool"));
  arguments.insert(QStringLiteral("binding_mode"), QStringLiteral("pinned-value"));
  arguments.insert(QStringLiteral("frozen_head"), QStringLiteral("h1"));
  arguments.insert(QStringLiteral("current_head"), QStringLiteral("h1"));
  arguments.insert(QStringLiteral("parameters"), QJsonObject::fromVariantMap(request.parameters));
  arguments.insert(QStringLiteral("inputs"), QJsonObject::fromVariantMap(request.inputs));
  const QJsonObject tool = paleo::capability::runChatTool(QStringLiteral("paleo.seismic_attribute_group"), arguments,
                                                         QStringLiteral("host-test"), QStringLiteral("doc-s"), dir.path());
  QCOMPARE(tool.value(QStringLiteral("envelopeSha256")).toString(),
           first.summary.value(QStringLiteral("envelopeSha256")).toString());

  LimitGuard guard(1);
  request.clientRequestId = QStringLiteral("s-budget");
  request.inputs.insert(QStringLiteral("path"), QStringLiteral("/other"));
  const ExecuteResult budget = paleo::capability::execute(request);
  QCOMPARE(budget.code, QStringLiteral("budget_exceeded"));
}

void TestCapabilityGateway::envelopeBatchDoesNotRejectCoherence() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  LimitGuard guard(1000);
  ExecuteRequest request;
  request.capabilityId = QStringLiteral("seismic.attributes.compute");
  request.contractVersion = 1;
  request.documentId = QStringLiteral("doc-mix");
  request.callerScope = QStringLiteral("host-test");
  request.clientRequestId = QStringLiteral("mix-batch");
  request.bindingMode = QStringLiteral("pinned-value");
  request.frozenHead = QStringLiteral("h");
  request.currentHead = QStringLiteral("h");
  request.publishIntent = QStringLiteral("compute-only");
  request.operationDir = dir.path();
  request.parameters.insert(QStringLiteral("ATTRIBUTES"),
                            QStringList{QStringLiteral("envelope"), QStringLiteral("coherence")});
  request.parameters.insert(QStringLiteral("SAMPLE_INTERVAL_MS"), 4.0);
  request.parameters.insert(QStringLiteral("COHERENCE_IL_HALF"), 0);
  request.parameters.insert(QStringLiteral("COHERENCE_XL_HALF"), 0);
  QVariantList traces;
  for (int t = 0; t < 4; ++t) {
    QVariantList trace;
    for (int i = 0; i < 8; ++i)
      trace.append(std::sin(0.3 * (t + i)));
    traces.append(QVariant(trace));
  }
  QVariantList volume;
  for (int i = 0; i < 16; ++i)
    volume.append(std::sin(0.2 * i));
  request.inputs.insert(QStringLiteral("traces"), traces);
  request.inputs.insert(QStringLiteral("full_trace"), true);
  request.inputs.insert(QStringLiteral("volume"), volume);
  request.inputs.insert(QStringLiteral("nIl"), 2);
  request.inputs.insert(QStringLiteral("nXl"), 2);
  request.inputs.insert(QStringLiteral("nS"), 4);
  const ExecuteResult result = paleo::capability::execute(request);
  QVERIFY2(result.ok, qPrintable(result.code));
  QCOMPARE(result.code, QStringLiteral("computed"));
  QVERIFY(!result.summary.value(QStringLiteral("coherenceSha256")).toString().isEmpty());
  QCOMPARE(result.summary.value(QStringLiteral("plan")).toString(), QStringLiteral("split"));
}

void TestCapabilityGateway::constraintPlanAndBarriers() {
  QTemporaryDir dir;
  QVariantMap user;
  user.insert(QStringLiteral("FIELD"), QStringLiteral("z"));
  user.insert(QStringLiteral("CELL_SIZE"), 10.0);
  QString code;
  const QVariantMap direct = paleo::capability::effectiveScalarParams(QStringLiteral("paleo:paleo_constraint_idw"), 1, user, &code);
  QCOMPARE(code, QStringLiteral("ok"));
  QCOMPARE(direct.value(QStringLiteral("ANISO_RATIO")).toDouble(), 2.0);
  ExecuteRequest request;
  request.capabilityId = QStringLiteral("constraint.idw");
  request.contractVersion = 1;
  request.documentId = QStringLiteral("doc-c");
  request.callerScope = QStringLiteral("host-test");
  request.clientRequestId = QStringLiteral("idw");
  request.bindingMode = QStringLiteral("follow-object-head");
  request.frozenHead = QStringLiteral("layer-7");
  request.currentHead = QStringLiteral("layer-7");
  request.operationDir = dir.path();
  request.parameters = user;
  request.inputs.insert(QStringLiteral("points_ref"), QStringLiteral("ver-points"));
  request.inputs.insert(QStringLiteral("subinterval_id"), QStringLiteral("sub-1"));
  request.inputs.insert(QStringLiteral("constraints_ref"), QStringLiteral("ver-lines"));
  const ExecuteResult planned = paleo::capability::execute(request);
  QVERIFY2(planned.ok, qPrintable(planned.code));
  QCOMPARE(planned.summary.value(QStringLiteral("effective")).toMap().value(QStringLiteral("CELL_SIZE")).toDouble(), 10.0);
  QCOMPARE(planned.summary.value(QStringLiteral("kernel")).toString(),
           QStringLiteral("ConstraintIDWAlgorithm::processAlgorithm"));
  QCOMPARE(planned.commitState, QStringLiteral("not_requested"));
  QCOMPARE(planned.code, QStringLiteral("prepared"));
  QVERIFY(!planned.recomputed);
  QCOMPARE(planned.execution, QStringLiteral("prepared"));
  request.clientRequestId = QStringLiteral("idw-drop");
  request.inputs.insert(QStringLiteral("ignore_barriers"), true);
  const ExecuteResult dropped = paleo::capability::execute(request);
  QCOMPARE(dropped.code, QStringLiteral("barrier_downgrade_refused"));
  request.clientRequestId = QStringLiteral("idw-name");
  request.bindingMode = QStringLiteral("pinned-value");
  request.inputs.remove(QStringLiteral("ignore_barriers"));
  request.inputs.remove(QStringLiteral("points_ref"));
  const ExecuteResult missing = paleo::capability::execute(request);
  QCOMPARE(missing.code, QStringLiteral("missing_required"));
  request.clientRequestId = QStringLiteral("idw-rev");
  request.bindingMode = QStringLiteral("follow-object-head");
  request.inputs.clear();
  QCOMPARE(paleo::capability::execute(request).code, QStringLiteral("revision_not_a_value_store"));
}

void TestCapabilityGateway::intervalsStaySplit() {
  const QStringList boundaries{QStringLiteral("T70"), QStringLiteral("T71")};
  QCOMPARE(paleo::strat::classifyLabel(QStringLiteral("T70"), boundaries).kind, paleo::strat::LabelKind::Boundary);
  QCOMPARE(paleo::strat::classifyLabel(QStringLiteral("T70-T71"), boundaries).kind, paleo::strat::LabelKind::Interval);
  const auto scheme = paleo::strat::draftScheme(QStringLiteral("sch"), QStringLiteral("name"), boundaries, {},
                                                QStringLiteral("MD"));
  const auto options = paleo::strat::selectorOptions(scheme);
  QCOMPARE(options.size(), 1);
  QCOMPARE(options[0].subIntervalIds.size(), 4);
  QVERIFY(options[0].label != options[0].subIntervalIds[0]);
  QTemporaryDir dir;
  ExecuteRequest request = depthRequest(dir.path(), QStringLiteral("intervals"));
  QVariantList intervals;
  for (int i = 0; i < 4; ++i) {
    QVariantMap row;
    row.insert(QStringLiteral("intervalId"), options[0].subIntervalIds[i]);
    row.insert(QStringLiteral("occurrenceId"), QStringLiteral("occ-%1").arg(i));
    row.insert(QStringLiteral("kind"), QStringLiteral("interval"));
    intervals.append(row);
  }
  request.inputs.insert(QStringLiteral("intervals"), intervals);
  const ExecuteResult result = paleo::capability::execute(request);
  QVERIFY2(result.ok, qPrintable(result.code));
  QCOMPARE(result.summary.value(QStringLiteral("occurrenceIds")).toStringList().size(), 4);
  request.clientRequestId = QStringLiteral("surface");
  QVariantMap surface;
  surface.insert(QStringLiteral("intervalId"), QStringLiteral("T70"));
  surface.insert(QStringLiteral("occurrenceId"), QStringLiteral("s"));
  surface.insert(QStringLiteral("kind"), QStringLiteral("surface"));
  request.inputs.insert(QStringLiteral("intervals"), QVariantList{surface});
  QCOMPARE(paleo::capability::execute(request).code, QStringLiteral("surface_is_not_interval"));
}

void TestCapabilityGateway::toolProtocol() {
  QTemporaryDir dir;
  QJsonObject evil;
  evil.insert(QStringLiteral("actor"), QStringLiteral("root"));
  evil.insert(QStringLiteral("client_request_id"), QStringLiteral("x"));
  evil.insert(QStringLiteral("binding_mode"), QStringLiteral("pinned-value"));
  const QJsonObject refused = paleo::capability::runChatTool(QStringLiteral("paleo.depth_convert_batch"), evil,
                                                            QStringLiteral("host-test"), QStringLiteral("doc-1"), dir.path());
  QCOMPARE(refused.value(QStringLiteral("code")).toString(), QStringLiteral("untrusted_authority"));
  ExecuteRequest request = depthRequest(dir.path(), QStringLiteral("page"));
  QVariantList samples;
  for (int i = 0; i < 2000; ++i) {
    QVariantMap sample;
    sample.insert(QStringLiteral("sampleId"), QStringLiteral("sample-%1").arg(i));
    sample.insert(QStringLiteral("wellId"), QStringLiteral("W1"));
    sample.insert(QStringLiteral("occurrenceId"), QStringLiteral("occ"));
    sample.insert(QStringLiteral("time"), 150.0);
    samples.append(sample);
  }
  request.inputs.insert(QStringLiteral("samples"), samples);
  const ExecuteResult paged = paleo::capability::execute(request);
  QVERIFY(paged.ok);
  QVERIFY(paged.responseJson.size() <= 64 * 1024);
  QJsonParseError error;
  const QJsonObject page = QJsonDocument::fromJson(paged.responseJson, &error).object();
  QCOMPARE(error.error, QJsonParseError::NoError);
  QCOMPARE(page.value(QStringLiteral("code")).toString(), QStringLiteral("response_budget_exceeded"));
  QVERIFY(!paged.responseJson.contains("sample-1999"));
  const ExecuteResult unavailable = [&] {
    ExecuteRequest blocked = request;
    blocked.capabilityId = QStringLiteral("wellfacies.predict");
    blocked.clientRequestId = QStringLiteral("wf");
    blocked.parameters.clear();
    blocked.inputs.clear();
    blocked.inputs.insert(QStringLiteral("points_ref"), QStringLiteral("well-1"));
    return paleo::capability::execute(blocked);
  }();
  QCOMPARE(unavailable.code, QStringLiteral("unavailable"));
}

void TestCapabilityGateway::planAndBudgetClaims() {
  QVector<PlanNode> nodes{{QStringLiteral("depth"), QStringLiteral("depth.convert_batch"), 1, QStringLiteral("DEPTH")},
                          {QStringLiteral("seis"), QStringLiteral("seismic.attributes.compute"), 1, QStringLiteral("RMS")}};
  const auto order = paleo::capability::checkPlan(nodes, {});
  QCOMPARE(order.code, QStringLiteral("ok"));
  QCOMPARE(order.message, QStringLiteral("depth,seis"));
  QVector<PlanEdge> cycle{{QStringLiteral("depth"), QStringLiteral("DEPTH"), QStringLiteral("seis"), QStringLiteral("RMS")},
                          {QStringLiteral("seis"), QStringLiteral("RMS"), QStringLiteral("depth"), QStringLiteral("DEPTH")}};
  QCOMPARE(paleo::capability::checkPlan(nodes, cycle).code, QStringLiteral("port_type"));
  QCOMPARE(paleo::capability::checkPlan({{QStringLiteral("nope"), QStringLiteral("missing.cap"), 1, {}}}, {}).code,
           QStringLiteral("unknown_capability"));

  LimitGuard guard(1000);
  std::atomic<int> wins{0};
  std::atomic<int> losses{0};
  auto attempt = [&] {
    if (ProcessBudgetLedger::instance().tryReserve(800))
      wins.fetch_add(1);
    else
      losses.fetch_add(1);
  };
  std::thread a(attempt);
  std::thread b(attempt);
  a.join();
  b.join();
  QCOMPARE(wins.load() + losses.load(), 2);
  QCOMPARE(wins.load(), 1);
  ProcessBudgetLedger::instance().release(800);
  QVERIFY(ProcessBudgetLedger::instance().tryReserve(800));
  ProcessBudgetLedger::instance().release(800);

  QTemporaryDir dir;
  OperationLog log(dir.path());
  std::atomic<int> created{0};
  std::atomic<int> replayed{0};
  auto accept = [&] {
    const auto result = log.accept(QStringLiteral("d"), QStringLiteral("s"), QStringLiteral("k"), QStringLiteral("h"));
    if (result.code == QLatin1String("created"))
      created.fetch_add(1);
    else if (result.code == QLatin1String("replay"))
      replayed.fetch_add(1);
  };
  std::thread c(accept);
  std::thread d(accept);
  c.join();
  d.join();
  QCOMPARE(created.load(), 1);
  QCOMPARE(replayed.load(), 1);
}

void TestCapabilityGateway::reviewFixes() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  ExecuteRequest missingTime = depthRequest(dir.path(), QStringLiteral("no-time"));
  QVariantMap sample = missingTime.inputs.value(QStringLiteral("samples")).toList().first().toMap();
  sample.remove(QStringLiteral("time"));
  missingTime.inputs.insert(QStringLiteral("samples"), QVariantList{sample});
  const ExecuteResult omitted = paleo::capability::execute(missingTime);
  QVERIFY2(omitted.ok, qPrintable(omitted.code));
  const QVariantMap row = omitted.summary.value(QStringLiteral("samples")).toList().first().toMap();
  QCOMPARE(row.value(QStringLiteral("code")).toString(), QStringLiteral("invalid_time"));
  QVERIFY(!row.contains(QStringLiteral("depthM")));

  OperationLog first(dir.path() + QStringLiteral("/shared"));
  OperationLog second(dir.path() + QStringLiteral("/shared"));
  const auto created = first.accept(QStringLiteral("doc"), QStringLiteral("scope"), QStringLiteral("req"), QStringLiteral("hash"));
  QCOMPARE(created.code, QStringLiteral("created"));
  const auto replay = second.accept(QStringLiteral("doc"), QStringLiteral("scope"), QStringLiteral("req"), QStringLiteral("hash"));
  QCOMPARE(replay.code, QStringLiteral("replay"));
  QCOMPARE(replay.record.operationId, created.record.operationId);

  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  ExecuteRequest late = depthRequest(dir.path() + QStringLiteral("/late"), QStringLiteral("late"));
  late.publishIntent = QStringLiteral("publish");
  late.catalog = &catalog;
  late.projectDir = dir.path();
  int flips = 0;
  late.cancelled = [&flips] { return ++flips > 2; };
  const int before = catalog.versions().size();
  const ExecuteResult cancelled = paleo::capability::execute(late);
  QCOMPARE(cancelled.code, QStringLiteral("cancelled"));
  QCOMPARE(catalog.versions().size(), before);

  ExecuteRequest ragged;
  ragged.capabilityId = QStringLiteral("seismic.attributes.compute");
  ragged.contractVersion = 1;
  ragged.documentId = QStringLiteral("doc-r");
  ragged.callerScope = QStringLiteral("host-test");
  ragged.clientRequestId = QStringLiteral("ragged");
  ragged.bindingMode = QStringLiteral("pinned-value");
  ragged.frozenHead = QStringLiteral("h");
  ragged.currentHead = QStringLiteral("h");
  ragged.operationDir = dir.path() + QStringLiteral("/ragged");
  ragged.parameters.insert(QStringLiteral("ATTRIBUTES"), QStringList{QStringLiteral("rms")});
  ragged.parameters.insert(QStringLiteral("SAMPLE_INTERVAL_MS"), 4.0);
  ragged.inputs.insert(QStringLiteral("traces"), QVariantList{QVariantList{1.0, 2.0}, QVariantList{1.0}});
  QCOMPARE(paleo::capability::execute(ragged).code, QStringLiteral("ragged_trace"));

  ExecuteRequest shape = ragged;
  shape.clientRequestId = QStringLiteral("shape");
  shape.parameters.insert(QStringLiteral("ATTRIBUTES"), QStringList{QStringLiteral("coherence")});
  shape.inputs.clear();
  shape.inputs.insert(QStringLiteral("volume"), QVariantList{1.0});
  shape.inputs.insert(QStringLiteral("nIl"), 2);
  shape.inputs.insert(QStringLiteral("nXl"), 2);
  shape.inputs.insert(QStringLiteral("nS"), 2);
  QCOMPARE(paleo::capability::execute(shape).code, QStringLiteral("invalid_shape"));
}

void TestCapabilityGateway::secondReviewFixes() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString hole = dir.path() + QStringLiteral("/cache-hole");
  QVERIFY(QDir().mkpath(hole + QStringLiteral("/numeric-cache.json")));
  OperationLog first(hole);
  const auto created = first.accept(QStringLiteral("doc"), QStringLiteral("scope"), QStringLiteral("req"),
                                    QStringLiteral("hash"));
  QCOMPARE(created.code, QStringLiteral("created"));
  OperationLog second(hole);
  const auto replay = second.accept(QStringLiteral("doc"), QStringLiteral("scope"), QStringLiteral("req"),
                                    QStringLiteral("hash"));
  QCOMPARE(replay.code, QStringLiteral("replay"));
  QCOMPARE(replay.record.operationId, created.record.operationId);

  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  ExecuteRequest publish = depthRequest(dir.path() + QStringLiteral("/receipt"), QStringLiteral("receipt"));
  publish.publishIntent = QStringLiteral("publish");
  publish.catalog = &catalog;
  publish.projectDir = dir.path();
  const int before = catalog.versions().size();
  ExecuteResult failed;
  {
    // accept、running、publishing 各写一次，提交后的 finish 是第四次。
    OperationWriteFault fault(3);
    failed = paleo::capability::execute(publish);
  }
  QVERIFY(!failed.ok);
  QCOMPARE(failed.code, QStringLiteral("receipt_persist_failed"));
  QCOMPARE(failed.commitState, QStringLiteral("committed"));
  QVERIFY(!failed.summary.value(QStringLiteral("receipt")).toString().isEmpty());
  QCOMPARE(catalog.versions().size(), before + 1);
  OperationLog stored(publish.operationDir);
  QCOMPARE(stored.recordById(failed.operationId).commitState, QStringLiteral("publishing"));
  const ExecuteResult again = paleo::capability::execute(publish);
  QCOMPARE(again.code, QStringLiteral("commit_uncertain"));
  QCOMPARE(catalog.versions().size(), before + 1);
  QCOMPARE(stored.cancel(failed.operationId), QStringLiteral("commit_uncertain"));
  QCOMPARE(catalog.versions().size(), before + 1);

  LimitGuard guard(512);
  ExecuteRequest mixed;
  mixed.capabilityId = QStringLiteral("seismic.attributes.compute");
  mixed.contractVersion = 1;
  mixed.documentId = QStringLiteral("doc-mix");
  mixed.callerScope = QStringLiteral("host-test");
  mixed.clientRequestId = QStringLiteral("mix");
  mixed.bindingMode = QStringLiteral("pinned-value");
  mixed.frozenHead = QStringLiteral("h");
  mixed.currentHead = QStringLiteral("h");
  mixed.operationDir = dir.path() + QStringLiteral("/mix");
  mixed.parameters.insert(QStringLiteral("ATTRIBUTES"),
                          QStringList{QStringLiteral("envelope"), QStringLiteral("coherence")});
  mixed.parameters.insert(QStringLiteral("SAMPLE_INTERVAL_MS"), 4.0);
  QVariantList trace;
  for (int i = 0; i < 8; ++i)
    trace.append(std::sin(0.2 * i));
  QVariantList volume;
  for (int i = 0; i < 128; ++i)
    volume.append(std::sin(0.05 * i));
  mixed.inputs.insert(QStringLiteral("traces"), QVariantList{trace});
  mixed.inputs.insert(QStringLiteral("full_trace"), true);
  mixed.inputs.insert(QStringLiteral("volume"), volume);
  mixed.inputs.insert(QStringLiteral("nIl"), 4);
  mixed.inputs.insert(QStringLiteral("nXl"), 4);
  mixed.inputs.insert(QStringLiteral("nS"), 8);
  QCOMPARE(paleo::capability::execute(mixed).code, QStringLiteral("budget_exceeded"));
}

void TestCapabilityGateway::splitCoherenceFileMatchesTiledInterior() {
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  LimitGuard guard(1000);
  const int nIl = 5;
  const int nXl = 8;
  const int nS = 6;
  const int count = nIl * nXl * nS;
  std::vector<float> volume(static_cast<std::size_t>(count));
  QVariantList values;
  for (int i = 0; i < count; ++i) {
    if (i == 7) {
      volume[static_cast<std::size_t>(i)] = std::numeric_limits<float>::quiet_NaN();
      values.append(QStringLiteral("nope"));
    } else {
      volume[static_cast<std::size_t>(i)] = std::sin(0.17 * i);
      values.append(volume[static_cast<std::size_t>(i)]);
    }
  }
  const auto plan = paleo::seisattr::planAttributeBudget(0, nS, 0, false, false, true, nIl, nXl, 1, 1, 1000, nS);
  QCOMPARE(QString::fromStdString(plan.code), QStringLiteral("split"));
  QVERIFY(plan.xlChunk > 0);
  QVERIFY(plan.xlChunk < nXl - 2);

  ExecuteRequest request;
  request.capabilityId = QStringLiteral("seismic.attributes.compute");
  request.contractVersion = 1;
  request.documentId = QStringLiteral("doc-tile");
  request.callerScope = QStringLiteral("host-test");
  request.clientRequestId = QStringLiteral("tile-file");
  request.bindingMode = QStringLiteral("pinned-value");
  request.frozenHead = QStringLiteral("h");
  request.currentHead = QStringLiteral("h");
  request.publishIntent = QStringLiteral("compute-only");
  request.operationDir = dir.path();
  request.parameters.insert(QStringLiteral("ATTRIBUTES"), QStringList{QStringLiteral("coherence")});
  request.parameters.insert(QStringLiteral("SAMPLE_INTERVAL_MS"), 4.0);
  request.parameters.insert(QStringLiteral("COHERENCE_IL_HALF"), 1);
  request.parameters.insert(QStringLiteral("COHERENCE_XL_HALF"), 1);
  request.parameters.insert(QStringLiteral("COHERENCE_TIME_HALF"), 1);
  request.parameters.insert(QStringLiteral("IL_SPACING"), 25.0);
  request.parameters.insert(QStringLiteral("XL_SPACING"), 12.5);
  request.inputs.insert(QStringLiteral("volume"), values);
  request.inputs.insert(QStringLiteral("nIl"), nIl);
  request.inputs.insert(QStringLiteral("nXl"), nXl);
  request.inputs.insert(QStringLiteral("nS"), nS);
  const ExecuteResult result = paleo::capability::execute(request);
  QVERIFY2(result.ok, qPrintable(result.code));
  QCOMPARE(result.summary.value(QStringLiteral("tiled")).toBool(), true);

  paleo::seisattr::CoherenceTile tile;
  tile.volume = volume.data();
  tile.nIl = nIl;
  tile.nXl = nXl;
  tile.nS = nS;
  tile.ilHalf = 1;
  tile.xlHalf = 1;
  tile.timeHalf = 1;
  tile.ilSpacing = 25;
  tile.xlSpacing = 12.5;
  const auto tiled = paleo::seisattr::computeCoherenceTiled(tile, paleo::seisattr::CoherenceWeightMode::Equal, plan.xlChunk);
  QVERIFY2(tiled.ok, tiled.code.c_str());
  QFile file(result.summary.value(QStringLiteral("result_path")).toString());
  QVERIFY(file.open(QIODevice::ReadOnly));
  const QByteArray bytes = file.readAll();
  QCOMPARE(bytes.size(), static_cast<int>(tiled.interior.size() * sizeof(float)));
  QCOMPARE(bytes, QByteArray(reinterpret_cast<const char *>(tiled.interior.data()), bytes.size()));
}

void TestCapabilityGateway::chatHostCatalogPublishes() {
  QJsonObject pageObject;
  pageObject.insert(QStringLiteral("ok"), true);
  pageObject.insert(QStringLiteral("code"), QStringLiteral("computed"));
  pageObject.insert(QStringLiteral("operationId"), QStringLiteral("op-1"));
  pageObject.insert(QStringLiteral("result_path"), QStringLiteral("/tmp/owned.f32"));
  pageObject.insert(QStringLiteral("padding"), QString(8000, QLatin1Char('x')));
  const QByteArray page = paleo::capability::boundToolResponse(pageObject, 128);
  const QJsonObject trimmed = QJsonDocument::fromJson(page).object();
  QCOMPARE(trimmed.value(QStringLiteral("code")).toString(), QStringLiteral("response_budget_exceeded"));
  QCOMPARE(trimmed.value(QStringLiteral("result_path")).toString(), QStringLiteral("/tmp/owned.f32"));
  QCOMPARE(trimmed.value(QStringLiteral("operationId")).toString(), QStringLiteral("op-1"));

  QJsonObject prepare;
  prepare.insert(QStringLiteral("client_request_id"), QStringLiteral("prep"));
  prepare.insert(QStringLiteral("binding_mode"), QStringLiteral("pinned-value"));
  prepare.insert(QStringLiteral("parameters"), QJsonObject());
  prepare.insert(QStringLiteral("inputs"), QJsonObject());
  prepare.insert(QStringLiteral("publish_intent"), QStringLiteral("prepare"));
  const AiToolDispatch dispatch = dispatchAiTool(QStringLiteral("paleo.constraint_idw"), prepare);
  QCOMPARE(dispatch.status, AiToolDispatchStatus::Routed);

  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  DataCatalog catalog;
  QVERIFY(catalog.open(dir.path()));
  ExecuteRequest batch = depthRequest(dir.path() + QStringLiteral("/ops"), QStringLiteral("chat-pub"));
  QJsonObject arguments;
  arguments.insert(QStringLiteral("client_request_id"), QStringLiteral("chat-pub"));
  arguments.insert(QStringLiteral("binding_mode"), QStringLiteral("pinned-value"));
  arguments.insert(QStringLiteral("frozen_head"), QStringLiteral("h1"));
  arguments.insert(QStringLiteral("current_head"), QStringLiteral("forged-by-model"));
  arguments.insert(QStringLiteral("publish_intent"), QStringLiteral("publish"));
  arguments.insert(QStringLiteral("parameters"), QJsonObject::fromVariantMap(batch.parameters));
  arguments.insert(QStringLiteral("inputs"), QJsonObject::fromVariantMap(batch.inputs));
  const QJsonObject bare = paleo::capability::runChatTool(
      QStringLiteral("paleo.depth_convert_batch"), arguments, QStringLiteral("host-test"), QStringLiteral("doc-1"),
      dir.path() + QStringLiteral("/ops"), QStringLiteral("h1"));
  QCOMPARE(bare.value(QStringLiteral("code")).toString(), QStringLiteral("publish_unavailable"));
  const int before = catalog.versions().size();
  arguments.insert(QStringLiteral("client_request_id"), QStringLiteral("chat-pub-hosted"));
  const QJsonObject hosted = paleo::capability::runChatTool(
      QStringLiteral("paleo.depth_convert_batch"), arguments, QStringLiteral("host-test"), QStringLiteral("doc-1"),
      dir.path() + QStringLiteral("/ops"), QStringLiteral("h1"), {}, &catalog, dir.path());
  QCOMPARE(hosted.value(QStringLiteral("code")).toString(), QStringLiteral("committed"));
  QCOMPARE(catalog.versions().size(), before + 1);
}

QTEST_MAIN(TestCapabilityGateway)
#include "tst_capability_gateway.moc"
