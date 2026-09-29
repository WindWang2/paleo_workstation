// tst_pythonenv — PythonEnvService（路径契约 / 真实建 venv / 空 requirements
// 安装）与 MamclTool（路径推导 / 旗标 / 缺包失败链）。MAMCL 全链（解包 +
// 真实依赖 + GUI 启动）体量超出单测范畴，由手动验证兜底。
#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include "../src/services/pythonenv.h"
#include "../src/workflow/mamcltool.h"

class TestPythonEnv : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase() { qunsetenv("PALEO_MAMCL_ZIP"); }

    void paths()
    {
      QTemporaryDir tmp;
      PythonEnvService env(tmp.path());
      QCOMPARE(env.venvDir(QStringLiteral("mamcl")),
               tmp.path() + QStringLiteral("/venv/mamcl"));
      // 未创建时无解释器、未就绪
      QVERIFY(env.pythonExecutable(QStringLiteral("mamcl")).isEmpty());
      QVERIFY(!env.venvReady(QStringLiteral("mamcl")));
      QVERIFY(!env.isBusy());
    }

    void createVenvAndInstall()
    {
      if (PythonEnvService::findBasePython().isEmpty())
        QSKIP("no base python on PATH");
      QTemporaryDir tmp;
      PythonEnvService env(tmp.path());
      QSignalSpy spy(&env, &PythonEnvService::stepFinished);

      env.createVenv(QStringLiteral("t"));
      QVERIFY2(spy.wait(180000), "createVenv timed out");
      const QList<QVariant> created = spy.takeFirst();
      QCOMPARE(created.at(0).toString(), QStringLiteral("createVenv"));
      QVERIFY2(created.at(1).toBool(), qPrintable(created.at(2).toString()));
      QVERIFY(env.venvReady(QStringLiteral("t")));

      // 空 requirements：pip no-op，校验「venv 内 pip 可用」这条契约。
      QFile req(tmp.filePath(QStringLiteral("req.txt")));
      QVERIFY(req.open(QIODevice::WriteOnly));
      req.write("# nothing\n");
      req.close();
      env.installRequirements(QStringLiteral("t"), req.fileName());
      QVERIFY2(spy.wait(180000), "installRequirements timed out");
      const QList<QVariant> installed = spy.takeFirst();
      QCOMPARE(installed.at(0).toString(), QStringLiteral("installRequirements"));
      QVERIFY2(installed.at(1).toBool(), qPrintable(installed.at(2).toString()));
    }

    void mamclFailOnMissingZip()
    {
      QTemporaryDir tmp;
      PythonEnvService env(tmp.path());
      MamclTool tool(&env);
      tool.setPackageZip(tmp.path() + QStringLiteral("/nonexistent.zip"));
      QSignalSpy spy(&tool, &MamclTool::launchFinished);
      tool.open();
      QCOMPARE(spy.count(), 1); // 同步失败，不进状态机
      QCOMPARE(spy.first().at(0).toBool(), false);
      QVERIFY(!tool.isBusy());
    }

    void mamclPathsAndMarkers()
    {
      QTemporaryDir tmp;
      PythonEnvService env(tmp.path());
      MamclTool tool(&env);
      tool.setPackageZip(QStringLiteral("/data/MAMCL_v9.zip"));
      QCOMPARE(tool.packageDir(), tmp.path() + QStringLiteral("/pkg/MAMCL_v9"));
      QCOMPARE(tool.appScript(), tool.packageDir() + QStringLiteral("/app.py"));

      // 伪造已解包程序目录：app.py + requirements 就位但 venv 未建 → 不就绪。
      QDir().mkpath(tool.packageDir());
      QFile app(tool.appScript());
      QVERIFY(app.open(QIODevice::WriteOnly));
      app.write("# app\n");
      app.close();
      QFile req(tool.requirementsFile());
      QVERIFY(req.open(QIODevice::WriteOnly));
      req.write("numpy\n");
      req.close();
      QVERIFY(!tool.ready());
      // 依赖旗标钉在 venv 目录内，随 requirements 内容哈希命名。
      QVERIFY(tool.depsMarkerPath().startsWith(env.venvDir(QStringLiteral("mamcl"))));
      QVERIFY(tool.depsMarkerPath().contains(QStringLiteral(".paleo-deps-")));
    }
};

QTEST_GUILESS_MAIN(TestPythonEnv)
#include "tst_pythonenv.moc"
