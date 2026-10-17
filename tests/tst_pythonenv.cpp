// tst_pythonenv — PythonEnvService（路径契约 / 真实建 venv / 空 requirements
// 安装）与 MamclTool（路径推导 / 旗标 / 缺包失败链）。MAMCL 全链（解包 +
// 真实依赖 + GUI 启动）体量超出单测范畴，由手动验证兜底。
#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QCryptographicHash>

#include "../src/services/pythonenv.h"
#include "../src/workflow/mamcltool.h"

class TestPythonEnv : public QObject
{
  Q_OBJECT

  private slots:
    void initTestCase()
    {
      qunsetenv("PALEO_MAMCL_ZIP");
      qunsetenv("PALEO_MAMCL_ZIP_SHA256");
      qunsetenv("PALEO_MAMCL_ALLOW_UNPINNED");
    }

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

    // #142：外部程序包未登记 SHA-256 / 哈希不符 → 同步拒绝，不进解包。
    void mamclRejectsUnverifiedZip()
    {
      QTemporaryDir tmp;
      PythonEnvService env(tmp.path());
      MamclTool tool(&env);
      const QString zip = tmp.filePath(QStringLiteral("MAMCL_x.zip"));
      QFile f(zip);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("not really a zip");
      f.close();
      tool.setPackageZip(zip);
      QVERIFY(tool.expectedZipSha256().isEmpty()); // setPackageZip 不信编译期哈希
      {
        QSignalSpy spy(&tool, &MamclTool::launchFinished);
        tool.open();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toBool(), false);
        QVERIFY(spy.first().at(1).toString().contains(QStringLiteral("SHA-256")));
        QVERIFY(!tool.isBusy());
      }
      qputenv("PALEO_MAMCL_ZIP_SHA256", QByteArray(64, '0'));
      {
        QSignalSpy spy(&tool, &MamclTool::launchFinished);
        tool.open();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toBool(), false);
        QVERIFY(spy.first().at(1).toString().contains(QStringLiteral("不符")));
      }
      qunsetenv("PALEO_MAMCL_ZIP_SHA256");
      QCOMPARE(MamclTool::sha256OfFile(zip),
               QString::fromLatin1(QCryptographicHash::hash("not really a zip",
                                                            QCryptographicHash::Sha256).toHex()));
    }

    void mamclLockFileBesideZip()
    {
      QTemporaryDir tmp;
      PythonEnvService env(tmp.path());
      MamclTool tool(&env);
      tool.setPackageZip(QStringLiteral("/data/MAMCL_v9.zip"));
      QCOMPARE(tool.lockFile(), QStringLiteral("/data/MAMCL_v9.requirements.lock"));
    }

    // #479 挂死路径：假基底解释器（可执行 shell 桩）永不退出——超时与取消
    // 两条防线都必须把 stepFinished(false)+busyChanged(false) 送达，且随后
    // 新步骤照常可启（busy 不再锁死到重启）。POSIX-only（桩脚本不可执行于
    // Windows）。
    void stepTimesOutAndUnblocks()
    {
#ifdef Q_OS_UNIX
      QTemporaryDir tmp;
      const QString hanger = writeStub(tmp.path(), QStringLiteral("hanger"),
                                       QStringLiteral("#!/bin/sh\nexec sleep 300\n"));
      const QString quick = writeStub(tmp.path(), QStringLiteral("quick"),
                                      QStringLiteral("#!/bin/sh\nexit 0\n"));
      qputenv("PALEO_PYTHON", hanger.toUtf8());
      PythonEnvService env(tmp.path());
      env.setTimeoutMs(1500);
      QSignalSpy stepSpy(&env, &PythonEnvService::stepFinished);
      QSignalSpy busySpy(&env, &PythonEnvService::busyChanged);
      env.createVenv(QStringLiteral("t"));
      QVERIFY2(stepSpy.wait(15000), "hung step never timed out");
      const QList<QVariant> timedOut = stepSpy.takeFirst();
      QCOMPARE(timedOut.at(0).toString(), QStringLiteral("createVenv"));
      QCOMPARE(timedOut.at(1).toBool(), false);
      QVERIFY2(timedOut.at(2).toString().contains(QStringLiteral("超时")),
               qPrintable(timedOut.at(2).toString()));
      QVERIFY(!env.isBusy());
      QVERIFY(busySpy.count() >= 2);
      QCOMPARE(busySpy.last().at(0).toBool(), false);

      // busy 闸门真的复位：换上「秒退」桩，新步骤正常走完。
      qputenv("PALEO_PYTHON", quick.toUtf8());
      stepSpy.clear();
      env.createVenv(QStringLiteral("t2"));
      QVERIFY2(stepSpy.wait(15000), "service stayed busy after timeout");
      const QList<QVariant> next = stepSpy.takeFirst();
      QCOMPARE(next.at(1).toBool(), true);
      qunsetenv("PALEO_PYTHON");
#else
      QSKIP("hanging stub interpreter is Unix-specific");
#endif
    }

    void stepCancelAndUnblocks()
    {
#ifdef Q_OS_UNIX
      QTemporaryDir tmp;
      const QString hanger = writeStub(tmp.path(), QStringLiteral("hanger"),
                                       QStringLiteral("#!/bin/sh\nexec sleep 300\n"));
      const QString quick = writeStub(tmp.path(), QStringLiteral("quick"),
                                      QStringLiteral("#!/bin/sh\nexit 0\n"));
      qputenv("PALEO_PYTHON", hanger.toUtf8());
      PythonEnvService env(tmp.path());
      QSignalSpy stepSpy(&env, &PythonEnvService::stepFinished);
      QSignalSpy busySpy(&env, &PythonEnvService::busyChanged);
      env.createVenv(QStringLiteral("t"));
      QTRY_COMPARE(busySpy.count(), 1); // busyChanged(true) 已到
      env.cancel();
      QVERIFY2(stepSpy.wait(15000), "hung step never cancelled");
      const QList<QVariant> cancelled = stepSpy.takeFirst();
      QCOMPARE(cancelled.at(0).toString(), QStringLiteral("createVenv"));
      QCOMPARE(cancelled.at(1).toBool(), false);
      QVERIFY2(cancelled.at(2).toString().contains(QStringLiteral("取消")),
               qPrintable(cancelled.at(2).toString()));
      QVERIFY(!env.isBusy());
      QCOMPARE(busySpy.last().at(0).toBool(), false);

      qputenv("PALEO_PYTHON", quick.toUtf8());
      stepSpy.clear();
      env.createVenv(QStringLiteral("t2"));
      QVERIFY2(stepSpy.wait(15000), "service stayed busy after cancel");
      QCOMPARE(stepSpy.takeFirst().at(1).toBool(), true);
      qunsetenv("PALEO_PYTHON");
#else
      QSKIP("hanging stub interpreter is Unix-specific");
#endif
    }

  private:
    // 落一个带执行位的 shell 桩脚本，返回绝对路径（Unix 专用）。
    static QString writeStub(const QString &dir, const QString &name,
                             const QString &content)
    {
      const QString path = QDir(dir).filePath(name);
      QFile f(path);
      if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
      f.write(content.toUtf8());
      f.close();
      f.setPermissions(QFileDevice::ExeOwner | QFileDevice::ReadOwner |
                       QFileDevice::WriteOwner);
      return path;
    }
};

QTEST_GUILESS_MAIN(TestPythonEnv)
#include "tst_pythonenv.moc"
