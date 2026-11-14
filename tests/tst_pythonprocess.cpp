// 层：测试壳
#include "services/pythonprocess.h"
#include "services/pythonprocess_internal.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace paleo::python;
namespace {
QString sha(const QByteArray &bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
QString literal(const QString &value) {
  auto encoded=QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
  return QString::fromUtf8(encoded.mid(1,encoded.size()-2));
}
bool write(const QString &path,const QByteArray &bytes) {
  QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();
}
QByteArray read(const QString &path) {QFile file(path);return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();}
std::shared_ptr<const VerifiedRuntime> fixtureRuntime() {
  QString interpreter=qEnvironmentVariable("PALEO_PROCESS_FIXTURE_PYTHON");
#ifdef PALEO_PROCESS_FIXTURE_PYTHON
  if(interpreter.isEmpty())interpreter=QString::fromUtf8(PALEO_PROCESS_FIXTURE_PYTHON);
#endif
  // Only the test harness supplies the development interpreter. Production never discovers PATH.
  if(interpreter.isEmpty())interpreter=qEnvironmentVariable("PALEO_PYTHON");
  interpreter=QFileInfo(interpreter).canonicalFilePath();
  if(interpreter.isEmpty()||!QFileInfo(interpreter).isExecutable())return {};
  auto runtime=std::make_shared<VerifiedRuntime>();runtime->root=QFileInfo(interpreter).canonicalPath();
  runtime->interpreter=interpreter;runtime->platform=runtimePlatform();runtime->runtimeVersion="test-harness-interpreter";
  return runtime;
}
TrustedProcessRequest requestFor(const std::shared_ptr<const VerifiedRuntime> &runtime,const QString &path,const QByteArray &code) {
  TrustedProcessRequest request;request.runtime=runtime;request.scriptPath=path;request.scriptSha256=sha(code);
  request.policy.allowTrusted=true;return request;
}
QByteArray heartbeatCode(const QString &heartbeat,const QString &ready,bool spawnChild=true) {
  if(!spawnChild)return QString("import pathlib,time\np=pathlib.Path(%1)\npathlib.Path(%2).write_text('ready')\n"
    "for i in range(10000):\n p.write_text(str(i))\n time.sleep(0.01)\n").arg(literal(heartbeat),literal(ready)).toUtf8();
  return QString("import pathlib,time,subprocess,sys,os\n"
    "heartbeat=pathlib.Path(%1)\nready=pathlib.Path(%2)\n"
    "child=\"import pathlib,time; p=pathlib.Path(\"+repr(str(heartbeat))+\"); "
    "exec('for i in range(10000):\\n p.write_text(str(i))\\n time.sleep(0.01)')\"\n"
    "try:\n subprocess.Popen([sys.executable,'-I','-u','-B','-c',child],start_new_session=(os.name!='nt'))\n"
    "except OSError:\n heartbeat.write_text('child-denied')\n"
    "ready.write_text('ready')\ntime.sleep(30)\n").arg(literal(heartbeat),literal(ready)).toUtf8();
}
int hostFixture(const QStringList &args) {
  if(args.size()!=6)return 91;
  auto runtime=std::make_shared<VerifiedRuntime>();runtime->interpreter=args[2];runtime->root=QFileInfo(args[2]).canonicalPath();
  auto request=requestFor(runtime,args[3],read(args[3]));
  if(request.scriptSha256!=args[4])return 92;
  request.policy.wallSeconds=30;
  const auto result=runTrustedProcess(request);return result.ok()?0:93;
}
} // namespace

class TestPythonProcess : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    m_runtime=fixtureRuntime();QVERIFY2(m_runtime,"Explicit test-harness Python interpreter must be configured");
  }
  void policyIdentityAndBudgetFailBeforeStart() {
    QTemporaryDir dir;QVERIFY(dir.isValid());const auto path=dir.filePath("fixed.py");
    const QByteArray code="print('trusted')\n";QVERIFY(write(path,code));auto request=requestFor(m_runtime,path,code);
    request.policy.allowTrusted=false;QCOMPARE(runTrustedProcess(request).code,QString("python_execution_denied"));
    request.policy.allowTrusted=true;request.scriptSha256=QString(64,'0');
    QCOMPARE(runTrustedProcess(request).code,QString("python_script_changed"));
    request.scriptSha256=sha(code);request.input="too big";request.policy.inputBytes=1;
    QCOMPARE(runTrustedProcess(request).code,QString("python_input_budget_exceeded"));
    request.input.clear();request.policy.codeBytes=1;QCOMPARE(runTrustedProcess(request).code,QString("python_script_invalid"));
    request.policy.codeBytes=256*1024;request.policy.threads=2;
    QCOMPARE(runTrustedProcess(request).code,QString("python_policy_invalid"));
  }
  void rejectsInvalidEncodingAndRelativePath() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py");const QByteArray code("\xff",1);QVERIFY(write(path,code));
    auto request=requestFor(m_runtime,path,code);QCOMPARE(runTrustedProcess(request).code,QString("python_script_encoding_invalid"));
    request.scriptPath="fixed.py";QCOMPARE(runTrustedProcess(request).code,QString("python_script_invalid"));
  }
  void rejectsTruncatedUtf8AtTheEndOfCompleteSource() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py");
    for(const auto &source:{QByteArray("#x\xc3",3),QByteArray("#x\xe2\x82",4),QByteArray("#x\xf0\x9f\x92",5)}) {
      QVERIFY(write(path,source));const auto request=requestFor(m_runtime,path,source);
      QCOMPARE(runTrustedProcess(request).code,QString("python_script_encoding_invalid"));
    }
  }
  void fixedSourceUsesCleanEnvironmentAndExactInput() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py");
    const QByteArray code="import sys,json,os\nraw=sys.stdin.buffer.read()\nsys.stderr.write('diagnostic')\n"
      "print(json.dumps({'input':raw.decode('utf8'),'isolated':sys.flags.isolated,'utf8':sys.flags.utf8_mode,"
      "'bytecode':sys.dont_write_bytecode,'secret':os.environ.get('PALEO_PROCESS_SECRET'),'path':os.environ.get('PATH')}))\n";
    QVERIFY(write(path,code));auto request=requestFor(m_runtime,path,code);request.input="a\n\"b\"\n";
    qputenv("PALEO_PROCESS_SECRET","must-not-leak");const auto result=runTrustedProcess(request);qunsetenv("PALEO_PROCESS_SECRET");
    QVERIFY2(result.ok(),qPrintable(result.code+':'+QString::fromUtf8(result.error)));
    const auto object=QJsonDocument::fromJson(result.output).object();QCOMPARE(object.value("input").toString(),QString::fromUtf8(request.input));
    QCOMPARE(object.value("isolated").toInt(),1);QCOMPARE(object.value("utf8").toInt(),1);QVERIFY(object.value("bytecode").toBool());
    QVERIFY(object.value("secret").isNull());QCOMPARE(object.value("path").toString(),QFileInfo(m_runtime->interpreter).absolutePath());
    QCOMPARE(result.error,QByteArray("diagnostic"));QVERIFY(result.cpuMicros>=0);QVERIFY(result.peakMemoryBytes>0);
  }
  void scriptReplacementAfterHashDoesNotReplaceExecutedBytes() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py");const QByteArray original="print('authenticated')\n";
    QVERIFY(write(path,original));auto request=requestFor(m_runtime,path,original);int checks=0;bool replaced=false;
    // Hold the host claim so cancellation checkpoints here surround script authentication,
    // rather than also observing time spent waiting for the process-wide slot.
    request.permit=claimExecutionPermit();QVERIFY(request.permit);
    request.cancelled=[&] {if(++checks==2)replaced=write(path,"print('replacement')\n");return false;};
    const auto result=runTrustedProcess(request);QVERIFY2(result.ok(),qPrintable(result.code));QVERIFY(replaced);
    QCOMPARE(result.output.trimmed(),QByteArray("authenticated"));
  }
  void stdinAndStdoutPumpTogether() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py");
    const QByteArray code="import sys\nsys.stdout.buffer.write(b'x'*262144)\nsys.stdout.flush()\n"
      "data=sys.stdin.buffer.read()\nsys.stdout.buffer.write(b'\\n'+str(len(data)).encode())\n";
    QVERIFY(write(path,code));auto request=requestFor(m_runtime,path,code);request.input=QByteArray(2*1024*1024,'i');
    const auto result=runTrustedProcess(request);QVERIFY2(result.ok(),qPrintable(result.code));
    QCOMPARE(result.output.left(262144),QByteArray(262144,'x'));QCOMPARE(result.output.mid(262144),QByteArray("\n2097152"));
  }
  void outputAndLogBudgets_data() {
    QTest::addColumn<bool>("stderrChannel");QTest::newRow("stdout")<<false;QTest::newRow("stderr")<<true;
  }
  void outputAndLogBudgets() {
    QFETCH(bool,stderrChannel);QTemporaryDir dir;const auto path=dir.filePath("fixed.py");
    const auto code=QByteArray("import sys\nsys.")+(stderrChannel?"stderr":"stdout")+".buffer.write(b'x'*1048576)\n";
    QVERIFY(write(path,code));auto request=requestFor(m_runtime,path,code);request.policy.outputBytes=32768;request.policy.logBytes=16384;
    const auto result=runTrustedProcess(request);
    QCOMPARE(result.code,QString(stderrChannel?"python_process_log_limit":"python_process_output_limit"));
    QVERIFY(result.output.isEmpty());QVERIFY(result.error.size()<=request.policy.logBytes);
    QVERIFY(result.outputLog.size()<=request.policy.logBytes);
    if(stderrChannel)QCOMPARE(result.error,QByteArray(request.policy.logBytes,'x'));
    else QCOMPARE(result.outputLog,QByteArray(request.policy.logBytes,'x'));
  }
  void nonzeroExitAndStartFailure() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py");const QByteArray code="import sys\nprint('partial')\nsys.exit(17)\n";
    QVERIFY(write(path,code));auto request=requestFor(m_runtime,path,code);const auto result=runTrustedProcess(request);
    QCOMPARE(result.code,QString("python_process_failed"));QCOMPARE(result.exitCode,17);QVERIFY(result.output.isEmpty());
    QCOMPARE(result.outputLog.trimmed(),QByteArray("partial"));
    auto absent=std::make_shared<VerifiedRuntime>(*m_runtime);absent->interpreter=dir.filePath("not-python.exe");request.runtime=absent;
    QVERIFY(!runTrustedProcess(request).ok());
  }
  void timeoutAndCancellation() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py");const QByteArray code="import time\ntime.sleep(30)\n";
    QVERIFY(write(path,code));auto request=requestFor(m_runtime,path,code);request.policy.wallSeconds=1;
    QElapsedTimer timer;timer.start();const auto timed=runTrustedProcess(request);
    QCOMPARE(timed.code,QString("python_process_timeout"));QVERIFY(timer.elapsed()<8000);QVERIFY(timed.output.isEmpty());
    request.policy.wallSeconds=30;timer.restart();request.cancelled=[&]{return timer.elapsed()>150;};
    const auto cancelled=runTrustedProcess(request);QCOMPARE(cancelled.code,QString("cancelled"));QVERIFY(cancelled.output.isEmpty());
  }
  void cpuAndMemoryAreActuallyLimited() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py");const QByteArray busy="while True: pass\n";
    QVERIFY(write(path,busy));auto request=requestFor(m_runtime,path,busy);request.policy.cpuSeconds=1;request.policy.wallSeconds=15;
    const auto cpu=runTrustedProcess(request);QCOMPARE(cpu.code,QString("python_process_cpu_limit"));QVERIFY(cpu.output.isEmpty());
    const QByteArray allocate="blocks=[]\ntry:\n while True: blocks.append(bytearray(8*1024*1024))\n"
      "except MemoryError:\n print('allocation-denied')\n";
    QVERIFY(write(path,allocate));request=requestFor(m_runtime,path,allocate);request.policy.memoryBytes=96*1024*1024;request.policy.wallSeconds=15;
    const auto memory=runTrustedProcess(request);
    QVERIFY2(memory.code=="python_process_memory_limit"||(memory.ok()&&memory.output.trimmed()=="allocation-denied"),qPrintable(memory.code));
    QVERIFY(memory.peakMemoryBytes>0);QVERIFY(memory.peakMemoryBytes<=request.policy.memoryBytes+8*1024*1024);
  }
  void oneGlobalSlotIsCancellableAndQueueTimeIsNotExecutionTime() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py");const QByteArray code="print('queued')\n";
    QVERIFY(write(path,code));auto request=requestFor(m_runtime,path,code);request.policy.wallSeconds=1;
    ProcessResult result;std::unique_ptr<QThread> worker;
    auto held=process_detail::acquireProcessPermit();QVERIFY(held);
    QElapsedTimer timer;timer.start();request.cancelled=[&]{return timer.elapsed()>100;};
    QCOMPARE(runTrustedProcess(request).code,QString("cancelled"));
    request.cancelled={};
    worker.reset(QThread::create([&]{result=runTrustedProcess(request);}));worker->start();
    QTest::qWait(1250);QVERIFY(!worker->isFinished());held.reset();
    QVERIFY(worker->wait(8000));QVERIFY2(result.ok(),qPrintable(result.code));QCOMPARE(result.output.trimmed(),QByteArray("queued"));
  }
  void hostClaimCanMoveToExecutionThread() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py");const QByteArray code="print('claimed')\n";
    QVERIFY(write(path,code));auto request=requestFor(m_runtime,path,code);ProcessResult result;
    std::unique_ptr<QThread> worker;
    request.permit=claimExecutionPermit();QVERIFY(request.permit);
    const auto timer=std::make_shared<QElapsedTimer>();timer->start();request.cancelled=[timer]{return timer->elapsed()>5000;};
    worker.reset(QThread::create([&]{result=runTrustedProcess(request);}));worker->start();
    QVERIFY(worker->wait(8000));QVERIFY2(result.ok(),qPrintable(result.code));QCOMPARE(result.output.trimmed(),QByteArray("claimed"));
    request.permit.reset();
  }
  void cancellationStopsDescendantsEvenAfterNewSession() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py"),heartbeat=dir.filePath("heartbeat"),ready=dir.filePath("ready");
    const auto code=heartbeatCode(heartbeat,ready);QVERIFY(write(path,code));auto request=requestFor(m_runtime,path,code);
    QElapsedTimer timer;timer.start();request.cancelled=[&]{return QFileInfo::exists(ready)&&timer.elapsed()>400;};
    const auto result=runTrustedProcess(request);
    QVERIFY2(result.code=="cancelled"||result.code=="python_process_process_limit",qPrintable(result.code));
    if(result.code=="cancelled")QVERIFY(QFileInfo::exists(ready));
    const auto last=read(heartbeat);QTest::qWait(300);QCOMPARE(read(heartbeat),last);
  }
  void hostDeathStopsTrustedProcessTree() {
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py"),heartbeat=dir.filePath("heartbeat"),ready=dir.filePath("ready");
#ifdef Q_OS_WIN
    // The Windows envelope refuses every descendant; exercise host death with its one child.
    const auto code=heartbeatCode(heartbeat,ready,false);
#else
    const auto code=heartbeatCode(heartbeat,ready);
#endif
    QVERIFY(write(path,code));QProcess host;
    host.start(QCoreApplication::applicationFilePath(),{"--trusted-host",m_runtime->interpreter,path,sha(code),"fixture"});
    QVERIFY(host.waitForStarted(5000));QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(ready)||host.state()==QProcess::NotRunning,10000);
    QVERIFY2(QFileInfo::exists(ready),qPrintable(QString::fromUtf8(host.readAllStandardError())));
    host.kill();QVERIFY(host.waitForFinished(5000));QTest::qWait(300);const auto last=read(heartbeat);
    QTest::qWait(300);QCOMPARE(read(heartbeat),last);
  }
  void realPinnedNumericalTemplateWhenConfigured() {
    const auto root=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT"),anchor=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256");
    if(root.isEmpty()){QVERIFY(anchor.isEmpty());return;}
    RuntimeProvider provider({root,anchor,true});const auto runtime=provider.inspect();QVERIFY2(runtime.ready(),qPrintable(runtime.code));
    QTemporaryDir dir;const auto path=dir.filePath("fixed.py");const QByteArray code="import numpy as n\nprint(float(n.arange(5,dtype=n.float64)@n.arange(5,dtype=n.float64)))\n";
    QVERIFY(write(path,code));const auto result=runTrustedProcess(requestFor(runtime.runtime,path,code));
    QVERIFY2(result.ok(),qPrintable(result.code+':'+QString::fromUtf8(result.error)));QCOMPARE(result.output.trimmed(),QByteArray("30.0"));
  }
private:
  std::shared_ptr<const VerifiedRuntime> m_runtime;
};

class TestPythonProcessUnavailable:public QObject {
  Q_OBJECT
public:
  ProcessResult probe;
private slots:
  void unsupportedEnvelopeFailsClosedWithoutOutput() {
    QVERIFY(!probe.ok());QVERIFY(probe.output.isEmpty());QCOMPARE(probe.exitCode,-1);
    QVERIFY(probe.code=="python_linux_helpers_untrusted" || probe.code=="python_linux_user_manager_missing" ||
      probe.code=="python_process_qt_unsupported" || probe.code=="python_process_unavailable");
    qInfo().noquote()<<"Execution suite unavailable:"<<probe.code;
  }
};
int main(int argc,char **argv) {
  QCoreApplication app(argc,argv);
  if(app.arguments().value(1)=="--trusted-host")return hostFixture(app.arguments());
  if(qEnvironmentVariable("PALEO_REQUIRE_AGENT_PYTHON_EXECUTION")!="1") {
    QTemporaryDir directory;const auto path=directory.filePath("probe.py");const QByteArray code="print('probe')\n";
    if(directory.isValid() && write(path,code)) {
      const auto result=runTrustedProcess(requestFor(fixtureRuntime(),path,code));
      const QStringList unavailable{"python_linux_helpers_untrusted","python_linux_user_manager_missing",
        "python_process_qt_unsupported","python_process_unavailable"};
      if(unavailable.contains(result.code)) {
        TestPythonProcessUnavailable test;test.probe=result;return QTest::qExec(&test,argc,argv);
      }
    }
  }
  TestPythonProcess test;return QTest::qExec(&test,argc,argv);
}
#include "tst_pythonprocess.moc"
