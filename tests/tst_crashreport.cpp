#include <QtTest>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "../src/services/crashreport.h"

#include <csignal>
#include <QDir>
#include <QFile>
#include <QFileInfo>

// wave4/runtime-resilience：崩溃报告机制（PALEO_QGIS_PLAN §38 留白，按「本地
// 优先」落地——不联网、不回传）。本测试覆盖三件事：
//   1) .running 会话旗标生命周期：启动写入、正常退出清除、残留=脏退出检测；
//   2) 报告文件生成与格式：<baseDir>/crash/YYYYmmdd-HHMMSS.txt，含时间戳/
//      信号/应用/Qt/QGIS 版本/工程路径/回溯（handler 与本测试共用同一 fd 落盘
//      路径——信号处理器本身不在单测里触发）；
//   3) 重启恢复提示文案：脏退出（有/无报告）与干净退出三态。
class TestCrashReport : public QObject
{
  Q_OBJECT

private slots:
  // 放在最前：任何 install 之前，未装处理器时写报告必须安全失败。
  void writeWithoutInstallIsRejected();
  void flagLifecycleStartClean();
  void dirtyExitDetectedWhenFlagSurvives();
  void reportGeneratedWithExpectedFormat();
  void reportRespectsProjectContext();
  void reportSameSecondCollision();
  void latestReportPicksNewest();
  void recoveryNoticeTextCoversStates();

private:
  static QStringList reportFiles(const QString &baseDir)
  {
    return QDir(CrashReport::crashDirFor(baseDir))
        .entryList({QStringLiteral("*.txt")}, QDir::Files, QDir::Name);
  }
};

void TestCrashReport::flagLifecycleStartClean()
{
  QTemporaryDir tmp;
  // 全新目录：无残留旗标 → 非脏退出；旗标随后在位；清除后消失。
  const CrashReport::SessionStart start = CrashReport::installCrashHandler(tmp.path());
  QVERIFY(!start.previousDirtyExit);
  QVERIFY(start.lastReportPath.isEmpty());
  QVERIFY(QFile::exists(CrashReport::crashDirFor(tmp.path()) + QStringLiteral("/.running")));
  QCOMPARE(CrashReport::crashDirFor(tmp.path()), QDir(tmp.path()).filePath(QStringLiteral("crash")));

  CrashReport::clearRunningFlag();
  QVERIFY(!QFile::exists(CrashReport::crashDirFor(tmp.path()) + QStringLiteral("/.running")));
}

void TestCrashReport::dirtyExitDetectedWhenFlagSurvives()
{
  QTemporaryDir tmp;
  CrashReport::SessionStart first = CrashReport::installCrashHandler(tmp.path());
  QVERIFY(!first.previousDirtyExit);
  // 模拟崩溃：不清旗标就「重启」——第二次 install 必须看到残留旗标。
  const CrashReport::SessionStart second = CrashReport::installCrashHandler(tmp.path());
  QVERIFY(second.previousDirtyExit);
  // 无报告文件时如实不带报告路径。
  QVERIFY(second.lastReportPath.isEmpty());
  CrashReport::clearRunningFlag();
}

void TestCrashReport::reportGeneratedWithExpectedFormat()
{
  QTemporaryDir tmp;
  CrashReport::installCrashHandler(tmp.path());
  CrashReport::setProjectContext(QStringLiteral("/definitely/not/a/real/project"));

  QVERIFY(CrashReport::writeReportForSignal(SIGSEGV));

  const QStringList files = reportFiles(tmp.path());
  QCOMPARE(files.size(), 1);
  // 文件名契约：YYYYmmdd-HHMMSS（同秒冲突后缀在另一用例覆盖）。
  QVERIFY(QRegularExpression(
              QStringLiteral("^\\d{8}-\\d{6}\\.txt$"))
              .match(files.first())
              .hasMatch());

  QFile f(CrashReport::crashDirFor(tmp.path()) + QLatin1Char('/') + files.first());
  QVERIFY(f.open(QIODevice::ReadOnly));
  const QString content = QString::fromUtf8(f.readAll());

  QVERIFY(content.contains(QStringLiteral("time:")));
  QVERIFY(QRegularExpression(QStringLiteral("time: \\d{8}-\\d{6}"))
              .match(content)
              .hasMatch());
  QVERIFY(content.contains(QStringLiteral("signal: 11 (SIGSEGV)")));
  QVERIFY(content.contains(QStringLiteral("app:")));
  QVERIFY(content.contains(QStringLiteral("qt:")));
  QVERIFY(content.contains(QStringLiteral("qgis:")));
  QVERIFY(content.contains(QStringLiteral("project: /definitely/not/a/real/project")));
  QVERIFY(content.contains(QStringLiteral("pid:")));
  // 回溯节：execinfo 平台（本 CI Linux）必有节头与至少一帧。
  QVERIFY(content.contains(QStringLiteral("--- backtrace ---")));
  QVERIFY(content.count(QLatin1Char('\n')) >= 8);
  // 报告写出后旗标仍在（崩溃不清旗标——这正是脏退出检测的依据）。
  QVERIFY(QFile::exists(CrashReport::crashDirFor(tmp.path()) + QStringLiteral("/.running")));

  // 脏退出 + 有报告 → 下次 install 报出该报告路径。
  const CrashReport::SessionStart dirty =
      CrashReport::installCrashHandler(tmp.path());
  QVERIFY(dirty.previousDirtyExit);
  QVERIFY(!dirty.lastReportPath.isEmpty());
  QCOMPARE(QFileInfo(dirty.lastReportPath).fileName(), files.first());
  CrashReport::clearRunningFlag();
}

void TestCrashReport::reportRespectsProjectContext()
{
  QTemporaryDir tmp;
  CrashReport::installCrashHandler(tmp.path());
  CrashReport::setProjectContext(QStringLiteral("/alpha"));
  QVERIFY(CrashReport::writeReportForSignal(SIGABRT));
  CrashReport::setProjectContext(QStringLiteral("/beta"));
  QVERIFY(CrashReport::writeReportForSignal(SIGFPE));
  QCOMPARE(reportFiles(tmp.path()).size(), 2);

  const QString dir = CrashReport::crashDirFor(tmp.path());
  bool sawAlpha = false, sawBeta = false, sawAbort = false, sawFpe = false;
  for (const QString &name : reportFiles(tmp.path()))
  {
    QFile f(dir + QLatin1Char('/') + name);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString content = QString::fromUtf8(f.readAll());
    if (content.contains(QStringLiteral("project: /alpha")))
      sawAlpha = true;
    if (content.contains(QStringLiteral("project: /beta")))
      sawBeta = true;
    if (content.contains(QStringLiteral("(SIGABRT)")))
      sawAbort = true;
    if (content.contains(QStringLiteral("(SIGFPE)")))
      sawFpe = true;
  }
  QVERIFY(sawAlpha);
  QVERIFY(sawBeta);
  QVERIFY(sawAbort);
  QVERIFY(sawFpe);
  CrashReport::clearRunningFlag();
}

void TestCrashReport::reportSameSecondCollision()
{
  QTemporaryDir tmp;
  CrashReport::installCrashHandler(tmp.path());
  // 同秒两份报告都必须落盘（第二份带后缀），不得互相覆盖。
  QVERIFY(CrashReport::writeReportForSignal(SIGSEGV));
  QVERIFY(CrashReport::writeReportForSignal(SIGSEGV));
  QVERIFY(reportFiles(tmp.path()).size() >= 2);
  CrashReport::clearRunningFlag();
}

void TestCrashReport::latestReportPicksNewest()
{
  QTemporaryDir tmp;
  CrashReport::installCrashHandler(tmp.path());
  const QString dir = CrashReport::crashDirFor(tmp.path());
  // 手工落两份可排序的报告（跨秒文件名），latest 必须挑新者。
  QFile older(dir + QStringLiteral("/20260101-000000.txt"));
  QVERIFY(older.open(QIODevice::WriteOnly));
  older.close();
  QFile newer(dir + QStringLiteral("/20260102-000000.txt"));
  QVERIFY(newer.open(QIODevice::WriteOnly));
  newer.close();
  QCOMPARE(QFileInfo(CrashReport::latestReportPath(tmp.path())).fileName(),
           QStringLiteral("20260102-000000.txt"));
  CrashReport::clearRunningFlag();
}

void TestCrashReport::writeWithoutInstallIsRejected()
{
  // 未 install（无已解析落盘目录）时写报告必须安全失败，不落任何文件。
  const QString nowhere = QStringLiteral("/nonexistent-paleo-crash-base");
  QVERIFY(!CrashReport::writeReportForSignal(SIGSEGV));
  QVERIFY(CrashReport::latestReportPath(nowhere).isEmpty());
}

void TestCrashReport::recoveryNoticeTextCoversStates()
{
  CrashReport::SessionStart dirtyWithReport;
  dirtyWithReport.previousDirtyExit = true;
  dirtyWithReport.lastReportPath = QStringLiteral("/x/crash/20260101-000000.txt");
  const QString withReport = CrashReport::recoveryNoticeText(dirtyWithReport);
  QVERIFY(!withReport.isEmpty());
  QVERIFY(withReport.contains(QStringLiteral("20260101-000000.txt")));

  CrashReport::SessionStart dirtyNoReport;
  dirtyNoReport.previousDirtyExit = true;
  const QString withoutReport = CrashReport::recoveryNoticeText(dirtyNoReport);
  QVERIFY(!withoutReport.isEmpty());
  QVERIFY(!withoutReport.contains(QStringLiteral(".txt")));

  CrashReport::SessionStart clean;
  QVERIFY(CrashReport::recoveryNoticeText(clean).isEmpty());
}

QTEST_MAIN(TestCrashReport)
#include "tst_crashreport.moc"
