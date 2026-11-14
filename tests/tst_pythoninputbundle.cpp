// 层：测试壳
#include "services/pythoninputbundle.h"
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
using namespace paleo::python;

class TestPythonInputBundle:public QObject {
  Q_OBJECT
private slots:
  void frozenBytesAndControlMetadataAreContentAddressed() {
    QTemporaryDir project;ExecutionPolicy policy;const QMap<QString,QByteArray> files{{"code.py","print(1)"},{"request.json","{}"}};
    const auto first=freezeInputBundle(project.path(),files,{{"version","exact-value-1"}},policy);QVERIFY2(first.ok(),qPrintable(first.code));
    const auto second=freezeInputBundle(project.path(),files,{{"version","exact-value-1"}},policy);QVERIFY(second.ok());QCOMPARE(second.reference,first.reference);
    const auto third=freezeInputBundle(project.path(),files,{{"version","exact-value-2"}},policy);QVERIFY(third.ok());QVERIFY(third.reference!=first.reference);
    QVERIFY(verifyInputBundle(project.path(),first.reference,policy).ok());
  }
  void changedSameSizeAndInjectedFilesAreRejected() {
    QTemporaryDir project;ExecutionPolicy policy;
    const auto bundle=freezeInputBundle(project.path(),{{"code.py","print(1)"},{"request.json","{}"}},{},policy);QVERIFY(bundle.ok());
    QFile file(bundle.root+"/request.json");QVERIFY(file.open(QIODevice::WriteOnly));file.write("[]");file.close();
    QCOMPARE(verifyInputBundle(project.path(),bundle.reference,policy).code,QString("python_input_changed"));
    QVERIFY(file.open(QIODevice::WriteOnly));file.write("{}");file.close();
    QFile extra(bundle.root+"/unregistered.py");QVERIFY(extra.open(QIODevice::WriteOnly));extra.write("pass");extra.close();
    QCOMPARE(verifyInputBundle(project.path(),bundle.reference,policy).code,QString("python_input_changed"));
  }
  void pathsBudgetsAndCancellationFailBeforeActivation() {
    QTemporaryDir project;ExecutionPolicy policy;const QMap<QString,QByteArray> files{{"code.py","print(1)"},{"request.json","{}"}};
    auto unsafe=files;unsafe.insert("../escape","bad");QVERIFY(!freezeInputBundle(project.path(),unsafe,{},policy).ok());
    unsafe=files;unsafe.insert("aux.txt","bad");QCOMPARE(freezeInputBundle(project.path(),unsafe,{},policy).code,QString("python_input_path_unsafe"));
    policy.codeBytes=1;QCOMPARE(freezeInputBundle(project.path(),files,{},policy).code,QString("python_input_budget_exceeded"));
    policy={};policy.inputBytes=4;auto combined=files;combined.insert("data.bin","123");
    QCOMPARE(freezeInputBundle(project.path(),combined,{},policy).code,QString("python_input_budget_exceeded"));
    policy={};policy.diskBytes=8;
    QCOMPARE(freezeInputBundle(project.path(),files,{},policy).code,QString("python_input_disk_budget_exceeded"));
    policy={};QCOMPARE(freezeInputBundle(project.path(),files,{},policy,[]{return true;}).code,QString("cancelled"));
    QVERIFY(freezeInputBundle(project.path(),files,{},policy).ok());
  }
  void verificationDoesNotCreateMissingPrivateDirectories() {
    QTemporaryDir project;const QString hash(64,'a');
    const auto checked=verifyInputBundle(project.path(),{{"directory",".paleo/capability-ops/inputs/"+hash},{"manifestHash",hash}},{});
    QVERIFY(!checked.ok());QVERIFY(!QFileInfo::exists(project.path()+"/.paleo"));
  }
};
QTEST_GUILESS_MAIN(TestPythonInputBundle)
#include "tst_pythoninputbundle.moc"
