// 层：测试壳
#include "services/pythonsandbox_linux.h"
#include "services/pythonsandbox_linux_image.h"
#include "services/pythonprocess_linux_internal.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <fcntl.h>
#include <unistd.h>

using namespace paleo::python;
namespace {
QString sha(const QByteArray &bytes) {return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());}
bool write(const QString &path,const QByteArray &bytes) {QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();}
int descriptors() {return QDir("/proc/self/fd").entryList(QDir::AllEntries|QDir::NoDotAndDotDot).size();}
std::shared_ptr<VerifiedRuntime> runtime(const QString &root,const QByteArray &bytes) {
  if(!write(root+"/payload",bytes))return {};
  const QJsonObject row{{"path","payload"},{"size",bytes.size()},{"sha256",sha(bytes)},{"executable",false}};
  const auto raw=QJsonDocument(QJsonObject{{"files",QJsonArray{row}}}).toJson(QJsonDocument::Compact);
  if(!write(root+"/runtime-manifest.json",raw))return {};
  auto result=std::make_shared<VerifiedRuntime>();result->root=root;result->treeSize=bytes.size();result->manifestSha256=sha(raw);return result;
}
InputBundle inputs(const QString &root,const QString &protocol,const QByteArray &json="{}",const QByteArray &data="") {
  QMap<QString,QByteArray> values{{"code.py","print('ok')\n"},{"request.json",json}};
  if(protocol=="paleo.python-analysis-frame.v1")values.insert("data.bin",data);
  QJsonArray rows;
  for(auto it=values.begin();it!=values.end();++it) {
    if(!write(root+"/"+it.key(),it.value()))return {};
    rows.append(QJsonObject{{"path",it.key()},{"size",it.value().size()},{"sha256",sha(it.value())}});
  }
  const auto raw=QJsonDocument(QJsonObject{{"schema","paleo.python-inputs.v1"},
    {"metadata",QJsonObject{{"inputProtocol",protocol}}},{"files",rows}}).toJson(QJsonDocument::Compact);
  if(!write(root+"/manifest.json",raw))return {};
  InputBundle result;result.code="ok";result.root=root;result.reference={{"manifestHash",sha(raw)}};return result;
}
}
class PythonSandboxLinuxTest:public QObject {
  Q_OBJECT
private slots:
  void requiresActualControllerLimits() {
    QTemporaryDir dir;ExecutionPolicy policy;
    const auto check=[&]{return process_detail::linux_native::verifyResourceLimits(dir.path(),policy);};
    QVERIFY(!check().isEmpty());
    QVERIFY(write(dir.filePath("memory.max"),QByteArray::number(policy.memoryBytes)));
    QVERIFY(write(dir.filePath("memory.swap.max"),"0"));QVERIFY(write(dir.filePath("pids.max"),"8"));
    QVERIFY(write(dir.filePath("cpu.max"),"100000 100000"));QVERIFY(check().isEmpty());
    QVERIFY(write(dir.filePath("cpu.max"),"100001 100000"));QVERIFY(!check().isEmpty());
    QVERIFY(write(dir.filePath("cpu.max"),"max 100000"));QVERIFY(!check().isEmpty());
    QVERIFY(write(dir.filePath("cpu.max"),QByteArray(129,'1')));QVERIFY(!check().isEmpty());
    QVERIFY(write(dir.filePath("cpu.max"),"10000 100000"));QVERIFY(check().isEmpty());
    QVERIFY(write(dir.filePath("memory.max"),"max"));QVERIFY(!check().isEmpty());
    QVERIFY(write(dir.filePath("memory.max"),QByteArray::number(policy.memoryBytes)));
    QVERIFY(write(dir.filePath("memory.swap.max"),"1"));QVERIFY(!check().isEmpty());
    QVERIFY(write(dir.filePath("memory.swap.max"),"0"));QVERIFY(write(dir.filePath("pids.max"),"max"));QVERIFY(!check().isEmpty());
  }
  void sealedRuntimeRejectsWritesAndSourceChanges() {
    QTemporaryDir dir;QVERIFY(dir.isValid());const QByteArray original="authenticated-original";
    const auto source=runtime(dir.path(),original);QVERIFY(source);QString code;
    const auto image=snapshotLinuxRuntime(source,1024*1024,{},code);QVERIFY2(image,qPrintable(code));
    QVERIFY(write(dir.filePath("payload"),QByteArray(original.size(),'X')));
    QCOMPARE(image->files.size(),1);const auto fd=image->files.first().fd;
    QByteArray actual(original.size(),Qt::Uninitialized);QCOMPARE(::read(fd,actual.data(),actual.size()),original.size());QCOMPARE(actual,original);
    QCOMPARE(::fcntl(fd,F_GET_SEALS)&15,15);QCOMPARE(::write(fd,"X",1),-1);
    const auto changed=snapshotLinuxRuntime(source,1024*1024,{},code);QVERIFY(!changed);QCOMPARE(code,QString("python_runtime_changed"));
  }
  void expectedManifestCannotBeReplaced() {
    QTemporaryDir dir;const auto source=runtime(dir.path(),"original");QVERIFY(source);
    QVERIFY(write(dir.filePath("runtime-manifest.json"),"{}"));QString code;
    QVERIFY(!snapshotLinuxRuntime(source,1024*1024,{},code));QCOMPARE(code,QString("python_manifest_changed"));
  }
  void rejectsSymlinksAndClosesPartialCaptures() {
    QTemporaryDir dir;const auto source=runtime(dir.path(),"original");QVERIFY(source);QString code;
    const auto before=descriptors();QVERIFY(QFile::remove(dir.filePath("payload")));
    QVERIFY(QFile::link(dir.filePath("runtime-manifest.json"),dir.filePath("payload")));
    for(int i=0;i<8;++i)QVERIFY(!snapshotLinuxRuntime(source,1024*1024,{},code));
    QCOMPARE(code,QString("python_runtime_changed"));QCOMPARE(descriptors(),before);
    QVERIFY(!snapshotLinuxRuntime(source,1024*1024,[]{return true;},code));QCOMPARE(code,QString("cancelled"));
  }
  void inputFrameUsesAuthenticatedBytes() {
    QTemporaryDir dir;const QByteArray json="{\"shape\":[2]}",data=QByteArray::fromHex("0000803f00000040");
    const auto source=inputs(dir.path(),"paleo.python-analysis-frame.v1",json,data);QVERIFY(source.ok());QString code;ExecutionPolicy policy;
    const auto image=snapshotLinuxInputs(source,policy,1024*1024,1024*1024,{},code);QVERIFY2(image,qPrintable(code));
    QCOMPARE(image->stdinChunks.size(),3);QCOMPARE(qFromLittleEndian<quint32>(image->stdinChunks[0].constData()),quint32(json.size()));
    QCOMPARE(image->stdinChunks[1],json);QCOMPARE(image->stdinChunks[2],data);QCOMPARE(image->bufferedBytes,qint64(4+json.size()+data.size()));
    QVERIFY(write(dir.filePath("request.json"),QByteArray(json.size(),'X')));
    QVERIFY(!snapshotLinuxInputs(source,policy,1024*1024,1024*1024,{},code));QCOMPARE(code,QString("python_input_changed"));
  }
  void rejectsUnknownProtocolAndHostOverBudget() {
    QTemporaryDir dir;QString code;ExecutionPolicy policy;
    const auto unknown=inputs(dir.path(),"unknown");QVERIFY(unknown.ok());
    QVERIFY(!snapshotLinuxInputs(unknown,policy,1024*1024,1024*1024,{},code));QCOMPARE(code,QString("python_input_protocol_invalid"));
    const auto source=inputs(dir.path(),"paleo.python-json.v1");QVERIFY(source.ok());
    QVERIFY(!snapshotLinuxInputs(source,policy,1024*1024,1,{},code));QCOMPARE(code,QString("python_snapshot_memory_budget"));
    const auto oversized=inputs(dir.path(),"paleo.python-json.v1",QByteArray(65537,'x'));QVERIFY(oversized.ok());
    QVERIFY(!snapshotLinuxInputs(oversized,policy,1024*1024,1024*1024,{},code));QCOMPARE(code,QString("python_input_budget_exceeded"));
  }
};
QTEST_GUILESS_MAIN(PythonSandboxLinuxTest)
#include "tst_pythonsandbox_linux.moc"
