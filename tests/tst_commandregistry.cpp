// 层：测试壳
// U0 命令盘点：CommandRegistry 纯数据自检（无 QtWidgets、无主窗）。
#include <QtTest/QtTest>

#include "services/capabilityspec.h"
#include "services/commandspec.h"

using namespace paleo::command;

class TestCommandRegistry : public QObject {
  Q_OBJECT
private slots:
  void tableIsHealthy() {
    const QStringList errors = CommandRegistry::validate();
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    QVERIFY(CommandRegistry::all().size() >= 80);
  }

  void lookupByIdAndVersion() {
    const CommandSpec idw = CommandRegistry::spec(QStringLiteral("constraint.idw"));
    QVERIFY(idw.isValid());
    QCOMPARE(idw.kind, CommandKind::Compute);
    QCOMPARE(idw.capabilityId, QStringLiteral("constraint.idw"));
    QVERIFY(idw.hasSurface(SurfaceRibbon));
    QVERIFY(idw.hasSurface(SurfaceAgent));
    QVERIFY(!CommandRegistry::spec(QStringLiteral("constraint.idw"), 2).isValid());
    QVERIFY(!CommandRegistry::spec(QStringLiteral("no.such.command")).isValid());
    QVERIFY(CommandRegistry::contains(QStringLiteral("project.open")));
    QVERIFY(!CommandRegistry::contains(QString()));
  }

  void everyComputeCommandResolvesItsCapability() {
    int compute = 0;
    for (const CommandSpec &s : CommandRegistry::all()) {
      if (s.kind != CommandKind::Compute)
        continue;
      ++compute;
      const CapabilitySpec cap = CapabilityRegistry::spec(s.capabilityId, s.contractVersion);
      QVERIFY2(!cap.capabilityId.isEmpty(), qPrintable(s.commandId));
      QCOMPARE(s.commandId, cap.capabilityId);
    }
    QVERIFY(compute >= 4);
  }

  // Agent 位 = CapabilityRegistry::callableIds() 的子集，且四条 Callable 都可达。
  void agentSurfaceIsExactlyCallableCapabilities() {
    QStringList agent = CommandRegistry::agentCommandIds();
    QStringList callable = CapabilityRegistry::callableIds();
    agent.sort();
    callable.sort();
    QCOMPARE(agent, callable);
    QVERIFY(!agent.contains(QStringLiteral("wellfacies.predict")));
    QVERIFY(!agent.contains(QStringLiteral("facies.draft.publish")));
  }

  void stageAndSurfaceQueries() {
    const auto data = CommandRegistry::forStage(QStringLiteral("data"));
    QVERIFY(data.size() >= 10);
    for (const CommandSpec &s : data)
      QCOMPARE(s.stageId, QStringLiteral("data"));
    QVERIFY(CommandRegistry::forStage(QStringLiteral("nope")).isEmpty());
    const auto shortcuts = CommandRegistry::forSurface(SurfaceShortcut);
    for (const CommandSpec &s : shortcuts)
      QVERIFY2(!s.shortcutId.isEmpty(), qPrintable(s.commandId));
    // 页快捷键 Ctrl+1..5 全部登记。
    for (const char *page : {"data", "correlation", "predict", "seismic", "compose"})
      QCOMPARE(CommandRegistry::spec(QStringLiteral("view.stage.") + QLatin1String(page)).shortcutId,
               QStringLiteral("main.page.") + QLatin1String(page));
    QVERIFY(CommandRegistry::forDocKind(QStringLiteral("map")).isEmpty()); // U2 起才填 docKinds
  }

  void viewCommandsNeverReferenceCapabilities() {
    for (const CommandSpec &s : CommandRegistry::all())
      if (s.kind == CommandKind::View)
        QVERIFY2(s.capabilityId.isEmpty(), qPrintable(s.commandId));
  }

  void kindNames() {
    QCOMPARE(commandKindName(CommandKind::Compute), QStringLiteral("compute"));
    QCOMPARE(commandKindName(CommandKind::View), QStringLiteral("view"));
  }
};

QTEST_GUILESS_MAIN(TestCommandRegistry)
#include "tst_commandregistry.moc"
