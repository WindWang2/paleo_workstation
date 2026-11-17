// 层：测试壳
#include "fixtures/pythonenvironmentfixture.h"
#include "services/pythondelivery_files.h"
#include <QtTest>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <thread>

using namespace paleo::python;
using namespace python_environment_fixture;
class TestPythonEnvironmentConfig : public QObject {
  Q_OBJECT
private slots:
#ifdef Q_OS_WIN
  void hostPathsRejectRawDeviceAliasesBeforeNormalization() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates"));
    const auto path = fixture.configurationPath();
    const QStringList aliases{"\\\\?\\" + QDir::toNativeSeparators(path),
                              "//localhost/" + path.left(1) + "$" + path.mid(2)};
    for (const auto &alias : aliases) {
      PythonEnvironmentStore rejected(alias);
      QCOMPARE(rejected.load().code, QString("python_environment_config_path_invalid"));
      QCOMPARE(rejected.disable(0).code, QString("python_environment_config_path_invalid"));
      QVERIFY(!QFileInfo::exists(path));
    }
    const auto candidate = fixture.candidate("runtime");
    LegacyPythonEnvironment legacy{{"PALEO_AGENT_PYTHON_ROOT", "\\\\?\\" + QDir::toNativeSeparators(candidate.runtimeRoot)},
        {"PALEO_AGENT_PYTHON_MANIFEST_SHA256", candidate.runtimeSha256}, {"PALEO_AGENT_PYTHON_ENABLE_GENERATED", "1"}};
    QCOMPARE(legacyEnvironment(legacy).code, QString("python_environment_legacy_invalid"));
    PythonEnvironmentStore ordinary(QDir::toNativeSeparators(path));
    QVERIFY(ordinary.disable(0).ok()); QVERIFY(ordinary.load().ok());
  }
#endif
  void completeIdentityIsAtomicPersistentAndNeverStartsFixtureInterpreter() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); QVERIFY2(fixture.error.isEmpty(), qPrintable(fixture.error));
    const auto candidate = fixture.candidate("runtime"); PythonEnvironmentStore store(fixture.configurationPath());
    const auto activated = store.activate(candidate, 0); QVERIFY2(activated.ok(), qPrintable(activated.code));
    QCOMPARE(activated.configuration.revision, quint64(1)); QVERIFY(activated.configuration.enabled);
    const auto identity = activated.configuration.approved.first(); QCOMPARE(identity.runtimeSha256, candidate.runtimeSha256);
    QCOMPARE(identity.scriptsSha256, candidate.scriptsSha256); QVERIFY(!identity.legacyTemplates);
    PythonEnvironmentStore restarted(fixture.configurationPath()); const auto loaded = restarted.load();
    QVERIFY(loaded.ok()); QCOMPARE(loaded.configuration.json(), activated.configuration.json());
    QVERIFY(activated.verifiedRuntime->snapshot().ready()); // Seven nonexecutable-format bytes, no import probe.
  }
  void generatedOnlyNeedsNoTrustedPackageAndPartialSourcePairsFailClosed() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); auto candidate = fixture.candidate("runtime", "fixture", false, true);
    candidate.scriptsRoot.clear(); candidate.scriptsSha256.clear(); PythonEnvironmentStore store(fixture.configurationPath());
    auto activated = store.activate(candidate, 0); QVERIFY2(activated.ok(), qPrintable(activated.code));
    const auto scripts = activated.configuration.approved.first().json().value("scripts").toObject();
    QCOMPARE(scripts.value("mode").toString(), QString("none"));
    auto bad = candidate; bad.policy.allowTrusted = true; QVERIFY(!store.activate(bad, 1).ok());
    bad = candidate; bad.scriptsRoot = fixture.scriptsRoot; QVERIFY(!store.activate(bad, 1).ok());
    QCOMPARE(store.load().configuration.json(), activated.configuration.json());
  }
  void failedActivationAndRevisionConflictKeepPriorBytes() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); auto candidate = fixture.candidate("runtime");
    PythonEnvironmentStore store(fixture.configurationPath()); QVERIFY(store.activate(candidate, 0).ok());
    const auto previous = read(fixture.configurationPath()); auto bad = candidate; bad.runtimeSha256 = QString(64, '1');
    QVERIFY(!store.activate(bad, 1).ok()); QCOMPARE(read(fixture.configurationPath()), previous);
    QCOMPARE(store.activate(candidate, 0).code, QString("python_environment_config_changed"));
    QCOMPARE(read(fixture.configurationPath()), previous);
    bad = candidate; bad.scriptsSha256 = QString(64, '1'); QVERIFY(!store.activate(bad, 1).ok());
    QCOMPARE(read(fixture.configurationPath()), previous);
  }
  void configurationCannotMutateAnApprovedRuntimeTree() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); const auto candidate = fixture.candidate("runtime");
    PythonEnvironmentStore store(candidate.runtimeRoot + "/configuration.json");
    QCOMPARE(store.activate(candidate, 0).code, QString("python_environment_config_path_invalid"));
    QVERIFY(!QFileInfo::exists(store.path())); QVERIFY(!QFileInfo::exists(store.path() + ".lock"));
    QVERIFY(RuntimeProvider({candidate.runtimeRoot, candidate.runtimeSha256, true}).inspectIntegrity().ready());
  }
  void managedSourceWinsAndLegacyZeroOnlyNarrowsOneMode() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); const auto candidate = fixture.candidate("runtime");
    PythonEnvironmentStore store(fixture.configurationPath()); QVERIFY(store.activate(candidate, 0).ok());
    PythonEnvironmentRegistry registry(fixture.configurationPath());
    const LegacyPythonEnvironment legacy{{"PALEO_AGENT_PYTHON_ROOT", "/unrelated"}, {"PALEO_AGENT_PYTHON_ENABLE_GENERATED", "0"},
                                         {"PALEO_AGENT_PYTHON_ENABLE_TRUSTED", "1"}};
    QCOMPARE(registry.refresh(legacy, registry.beginRefresh()), QString());
    QCOMPARE(registry.status().value("source").toString(), QString("host-config"));
    QVERIFY(registry.status().value("allowTrusted").toBool()); QVERIFY(!registry.status().value("allowGenerated").toBool());
    QCOMPARE(registry.active(EnvironmentMode::Trusted).identity.runtimeRoot, QFileInfo(candidate.runtimeRoot).canonicalFilePath());
    QVERIFY(registry.disable(1).ok()); QVERIFY(!registry.status().value("allowTrusted").toBool());
    QCOMPARE(registry.refresh({}, registry.beginRefresh()), QString()); QVERIFY(!registry.status().value("allowGenerated").toBool());
  }
  void legacyCompiledTemplatesRemainSupportedWithoutManifest() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); const auto candidate = fixture.candidate("runtime");
    const LegacyPythonEnvironment environment{{"PALEO_AGENT_PYTHON_ROOT", candidate.runtimeRoot},
      {"PALEO_AGENT_PYTHON_MANIFEST_SHA256", candidate.runtimeSha256},
      {"PALEO_AGENT_PYTHON_SCRIPTS_ROOT", QFileInfo(QFINDTESTDATA("../vendor/agent-python/templates")).canonicalFilePath()},
      {"PALEO_AGENT_PYTHON_ENABLE_TRUSTED", "1"}};
    PythonEnvironmentRegistry registry(fixture.configurationPath());
    QCOMPARE(registry.refresh(environment, registry.beginRefresh()), QString());
    const auto selected = registry.active(EnvironmentMode::Trusted); QVERIFY2(selected.ok(), qPrintable(selected.code));
    QVERIFY(selected.identity.legacyTemplates); QVERIFY(selected.identity.scriptsSha256.isEmpty());
    QCOMPARE(registry.status().value("source").toString(), QString("legacy-env"));
    QVERIFY(!QFileInfo::exists(fixture.configurationPath()));
  }
  void invalidManagedConfigurationNeverFallsBackToLegacy() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); const auto candidate = fixture.candidate("runtime");
    QVERIFY(write(fixture.configurationPath(), "{\"unknown\":true}\n"));
    PythonEnvironmentRegistry registry(fixture.configurationPath());
    const LegacyPythonEnvironment legacy{{"PALEO_AGENT_PYTHON_ROOT", candidate.runtimeRoot},
      {"PALEO_AGENT_PYTHON_MANIFEST_SHA256", candidate.runtimeSha256}, {"PALEO_AGENT_PYTHON_ENABLE_GENERATED", "1"}};
    QCOMPARE(registry.refresh(legacy, registry.beginRefresh()), QString("python_environment_config_invalid"));
    QCOMPARE(registry.status().value("source").toString(), QString("host-config"));
    QVERIFY(!registry.active(EnvironmentMode::Generated).ok());
  }
  void ordinaryActivationRetainsOldModeTokensAndTrustedHistoryAfterRestart() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); PythonEnvironmentRegistry registry(fixture.configurationPath());
    const auto first = registry.activate(fixture.candidate("first"), 0); QVERIFY2(first.ok(), qPrintable(first.code));
    const auto old = registry.active(EnvironmentMode::Generated); QVERIFY(old.ok());
    const auto next = registry.activate(fixture.candidate("next", "changed"), 1); QVERIFY(next.ok());
    QVERIFY(!old.interrupted->load()); QVERIFY(!old.globalInterruption->load());
    QVERIFY(registry.resolve(old.identity.id, old.identity.runtimeSha256, old.identity.policy.json(), EnvironmentMode::Generated).ok());
    PythonEnvironmentRegistry restarted(fixture.configurationPath()); QCOMPARE(restarted.refresh({}, restarted.beginRefresh()), QString());
    auto restored = restarted.resolve(old.identity.id, old.identity.runtimeSha256, old.identity.policy.json(), EnvironmentMode::Generated);
    QVERIFY2(restored.ok(), qPrintable(restored.code)); QCOMPARE(restored.identity.runtimeRoot, old.identity.runtimeRoot);
    QCOMPARE(restarted.resolve(QString(64, 'a'), old.identity.runtimeSha256, old.identity.policy.json(), EnvironmentMode::Generated).code,
             QString("python_environment_identity_unapproved"));
  }
  void shrinkingOneGrantInterruptsOnlyThatModeAndRollbackDoesNotRegrant_data() {
    QTest::addColumn<bool>("revokeGenerated");
    QTest::newRow("generated-only-revoked") << true; QTest::newRow("trusted-only-revoked") << false;
  }
  void shrinkingOneGrantInterruptsOnlyThatModeAndRollbackDoesNotRegrant() {
    QFETCH(bool, revokeGenerated);
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); PythonEnvironmentRegistry registry(fixture.configurationPath());
    auto candidate = fixture.candidate("runtime"); QVERIFY(registry.activate(candidate, 0).ok());
    const auto generated = registry.active(EnvironmentMode::Generated), trusted = registry.active(EnvironmentMode::Trusted);
    if (revokeGenerated) candidate.policy.allowGenerated = false; else candidate.policy.allowTrusted = false;
    const auto next = registry.activate(candidate, 1); QVERIFY(next.ok());
    QCOMPARE(generated.interrupted->load(), revokeGenerated); QCOMPARE(trusted.interrupted->load(), !revokeGenerated);
    QCOMPARE(registry.active(EnvironmentMode::Trusted).ok(), revokeGenerated);
    QCOMPARE(registry.active(EnvironmentMode::Generated).ok(), !revokeGenerated);
    QVERIFY(registry.rollback(generated.identity.id, 2).ok());
    QCOMPARE(registry.status().value("allowGenerated").toBool(), !revokeGenerated);
    QCOMPARE(registry.status().value("allowTrusted").toBool(), revokeGenerated);
    QCOMPARE(generated.interrupted->load(), revokeGenerated); QCOMPARE(trusted.interrupted->load(), !revokeGenerated);
  }
  void frozenHistoryDoesNotNeedOriginalTemplatesButStillNeedsRuntimeIntegrity() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); PythonEnvironmentRegistry registry(fixture.configurationPath());
    QVERIFY(registry.activate(fixture.candidate("runtime"), 0).ok()); const auto old = registry.active(EnvironmentMode::Trusted);
    QVERIFY(write(fixture.scriptsRoot + "/NOTICE.md", "changed"));
    QVERIFY(!registry.refresh({}, registry.beginRefresh()).isEmpty());
    QVERIFY(!registry.active(EnvironmentMode::Trusted).ok()); QVERIFY(registry.active(EnvironmentMode::Generated).ok());
    QVERIFY(registry.resolve(old.identity.id, old.identity.runtimeSha256, old.identity.policy.json(), EnvironmentMode::Trusted).ok());
    const auto interpreter = old.runtime->snapshot().runtime->interpreter;
    QVERIFY(write(interpreter, "changed", true));
    QVERIFY(!registry.refresh({}, registry.beginRefresh()).isEmpty());
    QVERIFY(!registry.resolve(old.identity.id, old.identity.runtimeSha256, old.identity.policy.json(), EnvironmentMode::Trusted).ok());
  }
  void urgentDenySurvivesAnOlderActivationAndPersistenceFailure() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); PythonEnvironmentRegistry registry(fixture.configurationPath());
    QVERIFY(registry.activate(fixture.candidate("runtime"), 0).ok()); const auto old = registry.active(EnvironmentMode::Generated);
    const auto previous = read(fixture.configurationPath()); bool denied = false;
    const auto result = registry.activate(fixture.candidate("next", "changed"), 1, [&] {
      if (!denied) { denied = true; registry.denyInMemory(); } return false;
    });
    QVERIFY(denied); QVERIFY(!result.ok()); QVERIFY(old.interrupted->load() || old.globalInterruption->load());
    QVERIFY(!registry.status().value("allowGenerated").toBool()); QCOMPARE(read(fixture.configurationPath()), previous);
    { paleo::python::delivery::StoreLock lock(fixture.configurationPath() + ".lock"); QVERIFY(lock.locked());
      QCOMPARE(registry.disable(1).code, QString("python_environment_config_busy")); }
    QVERIFY(registry.status().value("emergencyDenied").toBool());
    QVERIFY(registry.status().value("inMemoryDeny").toBool()); QVERIFY(registry.status().value("enabled").toBool());
    QVERIFY(registry.activate(fixture.candidate("last", "changed"), 1).ok());
    QVERIFY(registry.active(EnvironmentMode::Generated).ok()); QVERIFY(old.interrupted->load() || old.globalInterruption->load());
  }
  void revokeIsDurableAndUnknownRecordPathsCannotCreateApproval() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); PythonEnvironmentRegistry registry(fixture.configurationPath());
    QVERIFY(registry.activate(fixture.candidate("runtime"), 0).ok()); const auto selected = registry.active(EnvironmentMode::Generated);
    registry.denyInMemory(selected.identity.id); QVERIFY(selected.interrupted->load());
    QVERIFY(registry.revoke(selected.identity.id, 1).ok());
    PythonEnvironmentRegistry restarted(fixture.configurationPath()); QCOMPARE(restarted.refresh({}, restarted.beginRefresh()), QString());
    QCOMPARE(restarted.resolve(selected.identity.id, selected.identity.runtimeSha256, selected.identity.policy.json(), EnvironmentMode::Generated).code,
             QString("python_environment_identity_revoked"));
    QCOMPARE(restarted.rollback(selected.identity.id, 2).code, QString("python_environment_identity_revoked"));
  }
  void ownerSnapshotsDoNotWaitForConcurrentRuntimeVerification() {
    Fixture fixture(QFINDTESTDATA("../vendor/agent-python/templates")); PythonEnvironmentRegistry registry(fixture.configurationPath());
    QVERIFY(registry.activate(fixture.candidate("runtime"), 0).ok()); const auto selected = registry.active(EnvironmentMode::Generated);
    std::atomic_bool entered{false}, release{false}; const auto revision = registry.beginRefresh();
    std::thread verifier([&] { registry.refresh({}, revision, [&] {
      if (selected.runtime->snapshot().code == "python_environment_verifying" && !entered.exchange(true)) {
        QElapsedTimer timer; timer.start();
        while (!release.load() && timer.elapsed() < 3000) QThread::msleep(1);
      }
      return false;
    }); });
    const auto cleanup = qScopeGuard([&] { release.store(true); verifier.join(); });
    QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 3000); QElapsedTimer timer; timer.start();
    QVERIFY(!registry.status().isEmpty()); QVERIFY(!registry.configurationSnapshot().isEmpty());
    QVERIFY(registry.active(EnvironmentMode::Generated).runtime);
    QCOMPARE(selected.runtime->snapshot().code, QString("python_environment_verifying"));
    QVERIFY(!selected.trusted->snapshot().value("executionReady").toBool());
    QVERIFY(!selected.sandbox->snapshot().value("ready").toBool());
    QVERIFY2(timer.elapsed() < 500, "Owner snapshots waited for the verifier instead of reporting pending");
  }
};
QTEST_GUILESS_MAIN(TestPythonEnvironmentConfig)
#include "tst_pythonenvironmentconfig.moc"
