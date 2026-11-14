// 层：测试壳
#include "services/pythonruntime.h"
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <atomic>
#include <thread>

using namespace paleo::python;
namespace {
QString sha(const QByteArray &bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
struct Fixture {
  QTemporaryDir temp;
  QString root;
  QJsonObject manifest;
  Fixture() {
    root=temp.path()+"/generation";QDir().mkpath(root);
#ifdef Q_OS_WIN
    const QString executable="python.exe";
#else
    const QString executable="python";
#endif
    QFile file(root+'/'+executable);file.open(QIODevice::WriteOnly);file.write("fixture");file.close();
    file.setPermissions(QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner|QFile::ReadGroup|QFile::ExeGroup|QFile::ReadOther|QFile::ExeOther);
    manifest={{"schema",QLatin1String(RuntimeSchema)},{"profile",QLatin1String(ProfileId)},
      {"runtimeVersion","cpython-3.14.10-numpy-2.5.4-1"},{"pythonVersion",QLatin1String(PythonVersion)},
      {"numpyVersion",QLatin1String(NumpyVersion)},{"platform",runtimePlatform()},{"interpreter",executable},
      {"sourceLockSha256",runtimeSourceLockSha256()},{"treeSize",7},
      {"files",QJsonArray{QJsonObject{{"path",executable},{"size",7},{"sha256",sha("fixture")},{"executable",true}}}}};
  }
  RuntimeConfig config() {
    const auto data=QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    QFile file(root+"/runtime-manifest.json");file.open(QIODevice::WriteOnly);file.write(data);file.close();
    return {root,sha(data),true};
  }
};
}
class TestPythonRuntime : public QObject {
  Q_OBJECT
private slots:
  void disabledAndNoAnchor() {
    QCOMPARE(verifyRuntime({}).code,QString("python_environment_disabled"));
    QCOMPARE(verifyRuntime({"/does-not-exist",{},true}).code,QString("python_manifest_anchor_required"));
    const auto json=verifyRuntime({}).json();QVERIFY(!json.value("ready").toBool());
    QVERIFY(!json.value("generatedCodeAvailable").toBool());QVERIFY(!json.contains("root"));
  }
  void verifiesTrustedManifestAndHoldsLease() {
    Fixture f;const auto config=f.config();auto first=verifyRuntime(config);QVERIFY2(first.ready(),qPrintable(first.code));
    QCOMPARE(first.runtime->treeSize,7);QCOMPARE(first.runtime->manifestSha256,config.manifestSha256);
    QCOMPARE(verifyRuntime(config).code,QString("python_runtime_busy"));
    auto retained=first.runtime;first={};QCOMPARE(verifyRuntime(config).code,QString("python_runtime_busy"));
    retained.reset();QVERIFY(verifyRuntime(config).ready());
  }
  void tamperingSameSizeAndUnexpectedFile() {
    Fixture f;const auto config=f.config();
    QFile file(f.root+'/'+f.manifest.value("interpreter").toString());QVERIFY(file.open(QIODevice::WriteOnly));file.write("changed");file.close();
    QCOMPARE(verifyRuntime(config).code,QString("python_runtime_changed"));
    QVERIFY(file.open(QIODevice::WriteOnly));file.write("fixture");file.close();
    QFile extra(f.root+"/injected.pth");QVERIFY(extra.open(QIODevice::WriteOnly));extra.write("import os");extra.close();
    QCOMPARE(verifyRuntime(config).code,QString("python_runtime_changed"));
  }
  void invalidManifests_data() {
    QTest::addColumn<QString>("kind");
    for(const auto &name:{"wrong-platform","wrong-source","wrong-version","traversal","duplicate","bad-size","wrong-interpreter","extra-key"})
      QTest::newRow(name)<<QString::fromLatin1(name);
  }
  void invalidManifests() {
    QFETCH(QString,kind);Fixture f;auto files=f.manifest.value("files").toArray();auto row=files.first().toObject();
    if(kind=="wrong-platform")f.manifest["platform"]="unknown";
    else if(kind=="wrong-source")f.manifest["sourceLockSha256"]=QString(64,'0');
    else if(kind=="wrong-version")f.manifest["pythonVersion"]="0.0.0";
    else if(kind=="traversal"){row["path"]="../outside";files[0]=row;}
    else if(kind=="duplicate")files.append(row);
    else if(kind=="bad-size"){row["size"]=1.5;files[0]=row;}
    else if(kind=="wrong-interpreter")f.manifest["interpreter"]="not-listed";
    else f.manifest["approved"]=true;
    f.manifest["files"]=files;QVERIFY(!verifyRuntime(f.config()).ready());
  }
  void manifestAnchorCannotBeReplacedBySelfReport() {
    Fixture f;auto config=f.config();f.manifest["runtimeVersion"]="changed";f.config();
    QCOMPARE(verifyRuntime(config).code,QString("python_manifest_changed"));
  }
  void cancellationDoesNotRetainGeneration() {
    Fixture f;const auto config=f.config();
    QCOMPARE(verifyRuntime(config,[]{return true;}).code,QString("cancelled"));
    QVERIFY(verifyRuntime(config).ready());
  }
  void policyOnlyShrinksAndNeverImplicitlyGrantsExecution() {
    ExecutionPolicy policy;QVERIFY(policy.validate().isEmpty());QVERIFY(!policy.allowTrusted);QVERIFY(!policy.allowGenerated);
    policy.outputBytes/=2;QVERIFY(policy.validate().isEmpty());policy.threads=2;
    QCOMPARE(policy.validate(),QString("python_policy_invalid"));policy.threads=1;policy.wallSeconds=121;
    QCOMPARE(policy.validate(),QString("python_policy_invalid"));
  }
  void snapshotsAndQueuedCancellationDoNotWaitForAnInspection() {
    Fixture fixture;RuntimeProvider provider(fixture.config());std::atomic_bool blocked=false;
    QSemaphore entered,release;
    std::thread worker([&]{provider.inspect([&]{
      if(!blocked.exchange(true)){entered.release();release.acquire();}
      return false;
    });});
    const auto cleanup=qScopeGuard([&]{release.release();worker.join();});
    QVERIFY(entered.tryAcquire(1,2000));
    QElapsedTimer elapsed;elapsed.start();
    QCOMPARE(provider.snapshot().code,QString("python_environment_verifying"));
    QCOMPARE(provider.inspect([]{return true;}).code,QString("cancelled"));
    QVERIFY(elapsed.elapsed()<500);
  }
  void realPinnedRuntimeWhenConfigured() {
    const auto root=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_ROOT");
    const auto anchor=qEnvironmentVariable("PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256");
    if(root.isEmpty()) {QVERIFY(anchor.isEmpty());QVERIFY(!RuntimeProvider({}).inspect().ready());return;}
    const auto previous=qgetenv("PATH");qputenv("PATH","untrusted-parent-search-path");
    const auto restore=qScopeGuard([previous]{qputenv("PATH",previous);});
    RuntimeProvider provider({root,anchor,true});auto first=provider.inspect();QVERIFY2(first.ready(),qPrintable(first.code));
    const auto second=provider.inspect();QVERIFY2(second.ready(),qPrintable(second.code));
    QVERIFY(provider.snapshot().ready());
  }
};
QTEST_GUILESS_MAIN(TestPythonRuntime)
#include "tst_pythonruntime.moc"
