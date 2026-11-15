// 层：测试壳
#include "services/piprocess.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QtTest>
#include <future>
#include <cstdio>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

using namespace paleo::pi;
namespace {
RuntimeConfig config(){return {true,qEnvironmentVariable("PALEO_TEST_PI_NATIVE_ROOT"),qEnvironmentVariable("PALEO_TEST_PI_NATIVE_SHA256")};}
RuntimeInspection identity(){return std::async(std::launch::async,[]{return inspectRuntime(config());}).get();}
qint64 nodeChild() {
#ifdef Q_OS_WIN
  const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return 0;
  PROCESSENTRY32W row{};row.dwSize=sizeof(row);qint64 found=0;
  if(Process32FirstW(snapshot,&row))do {
    if(row.th32ParentProcessID==GetCurrentProcessId()&&QString::fromWCharArray(row.szExeFile).compare("node.exe",Qt::CaseInsensitive)==0){found=row.th32ProcessID;break;}
  }while(Process32NextW(snapshot,&row));CloseHandle(snapshot);return found;
#else
  for(const auto &thread:QDir("/proc/self/task").entryList(QDir::Dirs|QDir::NoDotAndDotDot)) {
    QFile children("/proc/self/task/"+thread+"/children");if(!children.open(QIODevice::ReadOnly))continue;
    for(const auto &pid:children.readAll().trimmed().split(' ')) {
      const auto number=pid.toLongLong();if(number>0&&QFileInfo(QFileInfo("/proc/"+pid+"/exe").symLinkTarget()).fileName()=="node")return number;
    }
  }
  return 0;
#endif
}
struct ChildObserver {
  QElapsedTimer gap;int scans=0;qint64 scanMicros=0;
  ChildObserver(){gap.start();}
  qint64 poll() {
#ifdef Q_OS_WIN
    constexpr int interval=20;
#else
    constexpr int interval=2;
#endif
    if(gap.elapsed()<interval)return 0;
    QElapsedTimer cost;cost.start();const auto child=nodeChild();scanMicros+=cost.nsecsElapsed()/1000;++scans;
    // Enumeration itself can exceed the interval on Windows. Keep an actual gap
    // after the scan instead of enumerating at every runtime hash chunk.
    gap.restart();return child;
  }
};
bool running(qint64 pid) {
#ifdef Q_OS_WIN
  const auto handle=OpenProcess(SYNCHRONIZE,FALSE,DWORD(pid));if(!handle)return false;
  const bool alive=WaitForSingleObject(handle,0)==WAIT_TIMEOUT;CloseHandle(handle);return alive;
#else
  QFile state("/proc/"+QString::number(pid)+"/stat");if(!state.open(QIODevice::ReadOnly))return false;
  const auto raw=state.readAll();const auto end=raw.lastIndexOf(')');return end>=0&&end+2<raw.size()&&raw[end+2]!='Z';
#endif
}
bool terminateNode(qint64 pid) {
#ifdef Q_OS_WIN
  const auto handle=OpenProcess(PROCESS_TERMINATE,FALSE,DWORD(pid));if(!handle)return false;
  const auto ok=TerminateProcess(handle,19);CloseHandle(handle);return ok;
#else
  return ::kill(pid_t(pid),SIGKILL)==0;
#endif
}
bool suspendNode(qint64 pid) {
#ifdef Q_OS_WIN
  const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);if(snapshot==INVALID_HANDLE_VALUE)return false;
  THREADENTRY32 row{};row.dwSize=sizeof(row);bool found=false,ok=true;
  if(Thread32First(snapshot,&row))do {
    if(row.th32OwnerProcessID!=DWORD(pid))continue;found=true;
    const auto thread=OpenThread(THREAD_SUSPEND_RESUME,FALSE,row.th32ThreadID);
    if(!thread){ok=false;continue;}if(SuspendThread(thread)==DWORD(-1))ok=false;CloseHandle(thread);
  }while(Thread32Next(snapshot,&row));CloseHandle(snapshot);return found&&ok;
#else
  return ::kill(pid_t(pid),SIGSTOP)==0;
#endif
}
HandshakeRequest request(const std::shared_ptr<const VerifiedRuntime>& runtime) {
  return {true,runtime,"native_pi_a1","18446744073709551615",{}};
}
HandshakeResult worker(const HandshakeRequest& value){return std::async(std::launch::async,[value]{return runHandshake(value);}).get();}
}
class TestPiProcessNative:public QObject {
  Q_OBJECT
private slots:
  void realLockedHelloAndClose() {
    const auto runtime=identity();QVERIFY2(runtime.ok(),qPrintable(runtime.code));
    const auto result=worker(request(runtime.runtime));QVERIFY2(result.ok(),qPrintable(result.code+QString::fromUtf8(result.diagnostics)));
    QCOMPARE(result.peer.value("generation").toString(),QString("18446744073709551615"));
    QCOMPARE(result.peer.value("toolExecution").toString(),QString("sequential"));
    QCOMPARE(result.peer.value("modelTransport").toString(),QString("host-only-unconnected"));
    QVERIFY(!result.peer.value("sandboxed").toBool());
  }
  void processLocalLeaseAndExternalExclusion() {
    auto first=identity();QVERIFY2(first.ok(),qPrintable(first.code));
    auto second=identity();QVERIFY2(second.ok(),qPrintable(second.code));
    const auto external=[] {
      QProcess probe;probe.start(QCoreApplication::applicationFilePath(),{"--pi-lease-probe"});
      if(!probe.waitForFinished(10000))return QByteArray("probe_timeout");return probe.readAllStandardOutput().replace("\r\n","\n");
    };
    QCOMPARE(external(),QByteArray("pi_runtime_busy\n"));first.runtime.reset();
    QCOMPARE(external(),QByteArray("pi_runtime_busy\n"));second.runtime.reset();
    QCOMPARE(external(),QByteArray("ok\n"));
  }
  void retainedIdentityDoesNotEnableLaunch() {
    const auto runtime=identity();QVERIFY2(runtime.ok(),qPrintable(runtime.code));auto value=request(runtime.runtime);value.enabled=false;
    QCOMPARE(worker(value).code,QString("pi_disabled"));
  }
  void cancellationReapsFixedChild() {
    const auto runtime=identity();QVERIFY2(runtime.ok(),qPrintable(runtime.code));auto value=request(runtime.runtime);
    qint64 child=0;ChildObserver observer;value.cancelled=[&]{if(child)return true;child=observer.poll();return child>0;};
    const auto result=worker(value);qInfo()<<"observer scans"<<observer.scans<<"micros"<<observer.scanMicros;
    QCOMPARE(result.code,QString("cancelled"));QVERIFY(child>0);QVERIFY(result.peer.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!running(child),3000);
  }
  void unexpectedExitDoesNotReturnPeer() {
    const auto runtime=identity();QVERIFY2(runtime.ok(),qPrintable(runtime.code));auto value=request(runtime.runtime);
    qint64 child=0;bool terminated=false;ChildObserver observer;value.cancelled=[&] {
      if(child)return false;child=observer.poll();if(child)terminated=terminateNode(child);return false;
    };
    const auto result=worker(value);QVERIFY(child>0);QVERIFY(terminated);QCOMPARE(result.code,QString("pi_process_failed"));QVERIFY(result.peer.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!running(child),3000);
  }
  void deadlineReapsStoppedFixedChild() {
    const auto runtime=identity();QVERIFY2(runtime.ok(),qPrintable(runtime.code));auto value=request(runtime.runtime);
    qint64 child=0;bool suspended=false;ChildObserver observer;QElapsedTimer total;total.start();value.cancelled=[&] {
      if(child)return false;child=observer.poll();if(child)suspended=suspendNode(child);return false;
    };
    const auto result=worker(value);QVERIFY(child>0);QVERIFY(suspended);QCOMPARE(result.code,QString("pi_handshake_timeout"));
    QVERIFY(result.peer.isEmpty());QVERIFY(total.elapsed()<10000);QTRY_VERIFY_WITH_TIMEOUT(!running(child),3000);
  }
  void hostDeathReapsFixedChild() {
    QProcess host;host.start(QCoreApplication::applicationFilePath(),{"--pi-hostdeath"});QVERIFY(host.waitForStarted(3000));
    QByteArray output;QElapsedTimer clock;clock.start();
    while(!output.contains('\n')&&clock.elapsed()<10000&&host.state()!=QProcess::NotRunning){host.waitForReadyRead(25);output+=host.readAllStandardOutput();}
    const auto child=output.trimmed().toLongLong();QVERIFY2(child>0,host.readAllStandardError().constData());
    host.kill();QVERIFY(host.waitForFinished(3000));QTRY_VERIFY_WITH_TIMEOUT(!running(child),3000);
  }
};
int main(int argc,char** argv) {
  QCoreApplication application(argc,argv);const auto arguments=application.arguments();
  if(arguments.contains("--pi-lease-probe")) {
    const auto found=identity();std::printf("%s\n",found.code.toUtf8().constData());return found.ok()?0:1;
  }
  if(arguments.contains("--pi-hostdeath")) {
    const auto found=identity();if(!found.ok()){std::fprintf(stderr,"%s\n",found.code.toUtf8().constData());return 1;}
    auto value=request(found.runtime);bool notified=false;ChildObserver observer;value.cancelled=[&] {
      if(notified)return false;const auto child=observer.poll();if(child) {
        notified=true;std::printf("%lld\n",static_cast<long long>(child));std::fflush(stdout);QThread::msleep(300);
      }return false;
    };
    return worker(value).ok()?0:1;
  }
  TestPiProcessNative test;return QTest::qExec(&test,argc,argv);
}
#include "tst_piprocess_native.moc"
