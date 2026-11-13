// 层：测试壳
#include <QtTest>
#include "app/capabilityhostwiring.h"
#include "app/aiwiring.h"
#include "fixtures/documentsourcefixture.h"
#include "metadata/projectlock.h"
#include "metadata/documentsourcestore.h"
#include "workflow/aichatcontroller.h"
#include "workflow/commanddispatcher.h"
#include "workflow/depthconversionworkflow.h"
#include <paleo_data/object_descriptor.h>

using namespace paleo;
using namespace capability;
using namespace document_fixture;

namespace {
ExecuteRequest depth(const QString &requestId) {
  velmodel::VelocityWellControl well;
  well.wellId = "W1"; well.x = 0; well.y = 0; well.knots = {{100, 200}, {200, 500}};
  QString error;
  const auto model = velmodel::VelocityModel::fit({well}, velmodel::ModelType::IntervalAverage, &error);
  ExecuteRequest request;
  request.capabilityId = "depth.convert_batch"; request.clientRequestId = requestId;
  request.publishIntent = "compute-only"; request.bindingMode = "pinned-value";
  request.parameters = {{"TIME_SEMANTIC", "TWT"}, {"DEPTH_SEMANTIC", "TVD"},
    {"EXTRAPOLATION", "reject_outside"}, {"MODEL_REVISION", "rev-1"}};
  request.inputs = {{"model", model.toJson().toVariantMap()}, {"samples", QVariantList{
    QVariantMap{{"sampleId", "s1"}, {"wellId", "W1"}, {"occurrenceId", "o1"}, {"time", 150.0}}}}};
  return request;
}
void configure(CapabilityHost &service, Fixture &f, ProjectDirLock &lock) {
  configureCapabilityCallers(&service); service.setTaskService(&f.tasks);
  service.setBindingProvider([&f, &lock] { return captureCapabilityBinding(&f.session, &f.catalog, &lock, nullptr); });
  f.session.setHostContextProvider([&service] { return service.context(); });
}
}

class TestCapabilityHost : public QObject {
  Q_OBJECT
private slots:
  void productionBindingUsesPersistentDocumentAndActualLock() {
    Fixture f; QVERIFY(f.init()); QVERIFY(f.bind());
    ProjectDirLock lock(f.project);
    CapabilityHost service; configure(service, f, lock);
    QCOMPARE(service.context().documentId, QStringLiteral("fixture-document"));
    QVERIFY(service.context().documentId != f.project);
    QCOMPARE(service.prepare(depth("missing-lock"), host::Caller::Agent).code,
             QStringLiteral("project_write_ownership_required"));
    QVERIFY2(lock.tryLock(&f.error), qPrintable(f.error));
    QVERIFY(service.prepare(depth("bound"), host::Caller::Agent).ok);
    f.catalog.setLockedReadOnly(true);
    QCOMPARE(service.prepare(depth("readonly"), host::Caller::Agent).code, QStringLiteral("project_read_only"));
    auto publish = depth("not-opened"); publish.publishIntent = "publish";
    QCOMPARE(service.prepare(publish, host::Caller::Agent).code, QStringLiteral("project_read_only"));
  }

  void productionQueriesNeedNeitherWriteLockNorWritableProject() {
    Fixture f; QVERIFY(f.init()); QVERIFY(f.bind(true)); ProjectDirLock lock(f.project);
    CapabilityHost service; configure(service, f, lock);
    AiChatController chat; bindChatToolRunner(&chat, nullptr, nullptr, &f.catalog, f.project, f.session.documentId(), &service);
    const auto sequence = f.catalog.mutationSeq();
    QSignalSpy done(chat.toolRunner(), &AiChatToolRunner::toolFinished);
    chat.toolRunner()->run({"query-1", "paleo.query_project", "{\"topic\":\"summary\"}"});
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000); QVERIFY(done[0][1].toBool());
    QCOMPARE(f.catalog.mutationSeq(), sequence);
    f.session.close(); QVERIFY(f.bind(true)); // Same document, new epoch, runner has not rebound.
    chat.toolRunner()->run({"query-2", "paleo.query_project", "{\"topic\":\"summary\"}"});
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 2, 10000); QVERIFY(!done[1][1].toBool());
    const auto payload = QJsonDocument::fromJson(done[1][2].toString().toUtf8()).object();
    QCOMPARE(payload.value("code").toString(), QStringLiteral("stale_session"));
  }

  void preparedCannotGrantAuthorityOrSurviveContextChanges() {
    Fixture f; QVERIFY(f.init()); QVERIFY(f.bind()); ProjectDirLock lock(f.project); QVERIFY(lock.tryLock());
    CapabilityHost service; configure(service, f, lock);
    auto request = depth("original");
    const auto prepared = service.prepare(request, host::Caller::Agent); QVERIFY2(prepared.ok, qPrintable(prepared.code));
    auto changed = request; changed.parameters["MODEL_REVISION"] = "pretend-new";
    QCOMPARE(service.execute(prepared.handle, changed).code, QStringLiteral("prepared_tampered"));
    changed = request; changed.documentId = "other-project";
    QCOMPARE(service.prepare(changed, host::Caller::Agent).code, QStringLiteral("document_mismatch"));
    QCOMPARE(service.prepare(depth({}), host::Caller::Agent).code, QStringLiteral("client_request_id_required"));
    QVERIFY(service.prepare(depth({}), host::Caller::Ui).ok);
    f.session.close(); QVERIFY(f.bind());
    QCOMPARE(service.execute(prepared.handle, request).code, QStringLiteral("stale_session"));
    QVERIFY(f.tasks.tasks().isEmpty());
  }

  void revisionAndEffectsHaveDifferentMeanings() {
    host::CallContext state;
    state.open = state.persistentIdentity = state.ownsWrites = true; state.readOnly = false;
    state.documentId = "doc"; state.epoch = 7; state.allowedEffects = host::Read | host::PrivateStage;
    const auto frozen = state; ++state.catalogRevision; ++state.mutationSequence;
    QVERIFY(host::checkContext(state, frozen, host::Read | host::PrivateStage).isEmpty());
    QCOMPARE(host::checkContext(state, frozen, host::Read, true), QStringLiteral("stale_command"));
    QCOMPARE(host::checkContext(state, state, host::ExternalTransfer), QStringLiteral("effect_denied"));
    state.recoveryRequired = true;
    QVERIFY(host::checkContext(state, state, host::Read).isEmpty());
    QCOMPARE(host::checkContext(state, state, host::PrivateStage), QStringLiteral("recovery_required"));
    state.persistentIdentity = false;
    QVERIFY(host::checkContext(state, state, host::Read).isEmpty());
    QCOMPARE(host::checkContext(state, state, host::PrivateStage), QStringLiteral("persistent_identity_required"));
  }

  void dispatcherRechecksDisabledStateAndRequiresOpaqueTicket() {
    Fixture f; QVERIFY(f.init()); QVERIFY(f.bind()); ProjectDirLock lock(f.project); QVERIFY(lock.tryLock());
    CapabilityHost service; configure(service, f, lock);
    command::CommandDispatcher dispatcher; dispatcher.setCapabilityHost(&service);
    bool enabled = true;
    QVERIFY(dispatcher.bind("depth.convert_batch", [](const auto &, const auto &) {},
      [&enabled](const auto &) { command::CommandState s; s.enabled = enabled; return s; }));
    command::CommandContext ctx; ctx.documentId = f.session.documentId();
    const auto request = depth("ui");
    const auto plan = dispatcher.prepare("depth.convert_batch", ctx, request.parameters, request.inputs, "compute-only");
    QVERIFY2(plan.ok, qPrintable(plan.code)); QVERIFY(!plan.hostHandle.isEmpty());
    enabled = false;
    QCOMPARE(dispatcher.execute(plan).code, QStringLiteral("command_disabled"));
    enabled = true;
    auto forged = plan; forged.hostHandle = "invented";
    QCOMPARE(dispatcher.execute(forged).code, QStringLiteral("prepared_unknown"));
    auto mutated = plan; mutated.request.inputs.clear();
    QCOMPARE(dispatcher.execute(mutated).code, QStringLiteral("prepared_tampered"));
    enabled = false; mutated = plan; mutated.commandId = "unbound.command";
    QCOMPARE(dispatcher.execute(mutated).code, QStringLiteral("prepared_tampered"));
    enabled = true; dispatcher.unbind("depth.convert_batch");
    QCOMPARE(dispatcher.execute(plan).code, QStringLiteral("command_unbound"));
  }

  void productionGateAllowsOnlyTheMatchingRelocationRecovery() {
    Fixture f; QVERIFY(f.init());
    const auto version = f.external("original.txt", "unchanged bytes"); QVERIFY(!version.isEmpty()); QVERIFY(f.bind());
    const auto source = f.session.sourceForVersion(version);
    const QString moved = f.temp.filePath("moved.txt"); QVERIFY(document_fixture::write(moved, "unchanged bytes"));
    const QJsonObject intent{{"schema", 1}, {"source", sourceRefJson(source.ref)},
      {"parentVersionId", version}, {"assetId", source.assetId}, {"expectedCurrentVersion", version}, {"newPath", moved}};
    document::DocumentSourceStore sourceStore(f.meta);
    QVERIFY2(sourceStore.writeOperation(f.session.documentId(), "relocation-1", intent, &f.error), qPrintable(f.error));
    f.session.close(); QVERIFY(f.bind()); QVERIFY(f.session.recoveryRequired());
    ProjectDirLock lock(f.project); QVERIFY(lock.tryLock());
    CapabilityHost service; configure(service, f, lock);
    auto other = f.session.command(document::SourceCommand::Kind::RegisterBinding);
    QCOMPARE(f.session.execute(other).code, QStringLiteral("recovery_required"));
    auto command = f.session.command(document::SourceCommand::Kind::Relocate);
    command.source = source.ref; command.path = moved; command.operationId = "relocation-1";
    QSignalSpy finished(&f.session, &document::DocumentSession::commandFinished);
    const auto accepted = f.session.execute(command);
    QVERIFY2(accepted.pending || accepted.ok, qPrintable(accepted.code + ":" + accepted.error));
    if (accepted.pending) {
      QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 10000);
      const auto result = qvariant_cast<document::SourceCommandResult>(finished[0][0]);
      QVERIFY2(result.ok, qPrintable(result.error));
    }
    QVERIFY(!f.session.recoveryRequired());
  }

  void realSeismicReferenceIsConsumedBeforeWorkerValidation() {
    Fixture f; QVERIFY(f.init()); const auto file = f.temp.filePath("volume.sgy");
    QVERIFY(document_fixture::write(file, document_fixture::smallSegy())); QVERIFY(f.importModel(file)); QVERIFY(f.bind());
    const auto records = f.session.sources(); QCOMPARE(records.size(), 1);
    paleo::data::DataService data; std::string detail;
    QVERIFY(data.openProjectReadOnly(records[0].modelDirectory.toStdString(), detail));
    std::vector<paleo::data::DataEntry> volumes; QVERIFY(data.listData("volumes", 0, volumes, detail));
    QCOMPARE(volumes.size(), size_t(1)); paleo::data::DataObjectDescriptor object;
    QVERIFY(data.describeDataObject(paleo::data::DataObjectKind::SeismicVolume, volumes[0].id, object, detail));
    QVERIFY(object.revisionKnown); data.closeProject();
    const document::ModelObjectRef volume{records[0].ref, document::ObjectKind::SeismicVolume,
                                         object.id, QString::fromStdString(object.revision)};
    ProjectDirLock lock(f.project); QVERIFY(lock.tryLock()); CapabilityHost service; configure(service, f, lock);
    ExecuteRequest request; request.capabilityId = "seismic.attributes.compute";
    request.clientRequestId = "real-trace"; request.publishIntent = "compute-only";
    request.parameters = {{"ATTRIBUTES", QStringList{"rms"}}, {"WINDOW_HALF", 1}};
    request.inputs = {{"seismic_selection", QVariantMap{{"volume", document::modelObjectRefJson(volume).toVariantMap()},
      {"traceIndex", "2"}, {"firstSample", 3}, {"sampleCount", 2}}}};
    const auto prepared = service.prepare(request, host::Caller::Agent); QVERIFY2(prepared.ok, qPrintable(prepared.code));
    QVERIFY(!prepared.effectiveParameters.contains("SAMPLE_INTERVAL_MS"));
    bool done = false; ExecuteResult output; QString error;
    QVERIFY(service.start(prepared.handle, request, this, "trace", [&](auto r) { output = r; done = true; }, &error));
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000); QVERIFY2(output.ok, qPrintable(output.code));
    auto mixed = request; mixed.inputs["samples"] = QVariantList{1}; mixed.clientRequestId = "mixed";
    QCOMPARE(service.prepare(mixed, host::Caller::Agent).code, QStringLiteral("mixed_seismic_inputs"));
    mixed = request; mixed.capabilityId = "depth.convert_batch";
    QCOMPARE(service.prepare(mixed, host::Caller::Agent).code, QStringLiteral("mixed_seismic_inputs"));
  }

  void consumedTicketsDoNotExhaustPrepareCapacity() {
    Fixture f; QVERIFY(f.init()); QVERIFY(f.bind()); ProjectDirLock lock(f.project); QVERIFY(lock.tryLock());
    CapabilityHost service; configure(service, f, lock);
    for (int n = 0; n < 130; ++n) {
      const auto request = depth(QString::number(n));
      const auto prepared = service.prepare(request, host::Caller::Agent); QVERIFY2(prepared.ok, qPrintable(prepared.code));
      bool done = false; ExecuteResult output; QString error;
      QVERIFY(service.start(prepared.handle, request, this, "bounded", [&](auto r) { output = r; done = true; }, &error));
      QTRY_VERIFY_WITH_TIMEOUT(done, 10000); QVERIFY2(output.ok, qPrintable(output.code));
    }
  }

  void uiBatchAndChatUseSameProductionHostAndNumericKernel() {
    Fixture f; QVERIFY(f.init()); QVERIFY(f.bind()); ProjectDirLock lock(f.project); QVERIFY(lock.tryLock());
    CapabilityHost service; configure(service, f, lock);
    const auto request = depth("ui-request");
    command::CommandDispatcher dispatcher; dispatcher.setCapabilityHost(&service);
    command::CommandContext ctx; ctx.documentId = f.session.documentId(); ctx.clientRequestId = request.clientRequestId;
    const auto plan = dispatcher.prepare("depth.convert_batch", ctx, request.parameters, request.inputs, "compute-only");
    QVERIFY2(plan.ok, qPrintable(plan.code));
    ExecuteResult ui, batch; bool uiDone = false, batchDone = false; QString error;
    QSignalSpy triggered(&dispatcher, &command::CommandDispatcher::commandTriggered);
    QSignalSpy operations(&dispatcher, &command::CommandDispatcher::operationChanged);
    QVERIFY(dispatcher.start(plan, this, "UI", [&](auto r) { ui = r; uiDone = true; }, &error));
    QTRY_VERIFY_WITH_TIMEOUT(uiDone, 10000); QVERIFY2(ui.ok, qPrintable(ui.code));
    QCOMPARE(triggered.size(), 1); QCOMPARE(operations.size(), 1);
    QCOMPARE(triggered[0][1].toString(), service.callerScope(host::Caller::Ui));
    auto batchRequest = request; batchRequest.clientRequestId = "batch-request";
    const auto prepared = service.prepare(batchRequest, host::Caller::Batch); QVERIFY(prepared.ok);
    QVERIFY(service.start(prepared.handle, batchRequest, this, "Batch",
      [&](auto r) { batch = r; batchDone = true; }, &error));
    QTRY_VERIFY_WITH_TIMEOUT(batchDone, 10000); QVERIFY2(batch.ok, qPrintable(batch.code));
    AiChatController chat; bindChatToolRunner(&chat, nullptr, nullptr, &f.catalog, f.project, f.session.documentId(), &service);
    QSignalSpy done(chat.toolRunner(), &AiChatToolRunner::toolFinished);
    const QJsonObject args{{"client_request_id", "chat-request"}, {"binding_mode", "pinned-value"},
      {"publish_intent", "compute-only"}, {"parameters", QJsonObject::fromVariantMap(request.parameters)},
      {"inputs", QJsonObject::fromVariantMap(request.inputs)}};
    chat.toolRunner()->run({"tool-1", "paleo.depth_convert_batch",
      QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))});
    QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 10000); QVERIFY(done[0][1].toBool());
    const auto payload = QJsonDocument::fromJson(done[0][2].toString().toUtf8()).object();
    QCOMPARE(ui.summary.value("samples"), batch.summary.value("samples"));
    QCOMPARE(QJsonValue::fromVariant(ui.summary.value("samples")), payload.value("samples"));
  }
  void productionChatPublishesAndReplaysReadOnlyInTheSameSession() {
    Fixture f;QVERIFY(f.init());QVERIFY(f.bind());ProjectDirLock lock(f.project);QVERIFY(lock.tryLock());
    CapabilityHost service;configure(service,f,lock);AiChatController chat;
    bindChatToolRunner(&chat,nullptr,nullptr,&f.catalog,f.project,f.session.documentId(),&service);
    chat.toolRunner()->setResultSession("persistent-chat");const auto request=depth("published-chat");
    const QJsonObject args{{"client_request_id",request.clientRequestId},{"binding_mode","pinned-value"},{"publish_intent","publish"},
      {"parameters",QJsonObject::fromVariantMap(request.parameters)},{"inputs",QJsonObject::fromVariantMap(request.inputs)}};
    QSignalSpy done(chat.toolRunner(),&AiChatToolRunner::toolFinished);
    const auto run=[&](const QString &call){chat.toolRunner()->run({call,"paleo.depth_convert_batch",QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact))});};
    run("publish");QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000);QVERIFY(done[0][1].toBool());
    const auto first=QJsonDocument::fromJson(done[0][2].toString().toUtf8()).object();
    QCOMPARE(first.value("commitState").toString(),QString("committed"));QCOMPARE(first.value("contract").toString(),QString("paleo.operation.v2"));
    const int versions=f.catalog.versions().size();f.catalog.setLockedReadOnly(true);run("replay");
    QTRY_COMPARE_WITH_TIMEOUT(done.size(),2,10000);QVERIFY(done[1][1].toBool());
    const auto replay=QJsonDocument::fromJson(done[1][2].toString().toUtf8()).object();
    QCOMPARE(replay.value("operationId"),first.value("operationId"));QCOMPARE(replay.value("commitState").toString(),QString("committed"));
    QCOMPARE(f.catalog.versions().size(),versions);
  }
};

QTEST_GUILESS_MAIN(TestCapabilityHost)
#include "tst_capability_host.moc"
