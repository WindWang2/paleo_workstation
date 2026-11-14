// 层：测试壳
#include "workflow/evidencerecord.h"
#include <QCoreApplication>
#include <QScopeGuard>
#include <QtTest>

class TestEvidenceRuntime : public QObject {
  Q_OBJECT
private slots:
  void softwareDescribesOnlyAssembledRuntimeFacts() {
    auto *application=QCoreApplication::instance();QVERIFY(application);
    const auto previous=application->property("paleo.qgis.version");
    const auto restore=qScopeGuard([&]{application->setProperty("paleo.qgis.version",previous);});
    application->setProperty("paleo.qgis.version",QVariant());
    const auto without=paleo::workflow_detail::evidenceRecord(nullptr,{},{}).value("software").toMap();
    QVERIFY(!without.contains("qgis"));QCOMPARE(without.value("qt").toString(),QString::fromLatin1(qVersion()));
    application->setProperty("paleo.qgis.version",QStringLiteral("installed-runtime-fixture"));
    const auto installed=paleo::workflow_detail::evidenceRecord(nullptr,{},{}).value("software").toMap();
    QCOMPARE(installed.value("qgis").toString(),QString("installed-runtime-fixture"));
    application->setProperty("paleo.qgis.version",QVariant());
    QVERIFY(!paleo::workflow_detail::evidenceRecord(nullptr,{},{}).value("software").toMap().contains("qgis"));
    QCOMPARE(installed.value("qgis").toString(),QString("installed-runtime-fixture"));
    application->setProperty("paleo.qgis.version",QStringLiteral("  "));
    QVERIFY(!paleo::workflow_detail::evidenceRecord(nullptr,{},{}).value("software").toMap().contains("qgis"));
  }
};
QTEST_GUILESS_MAIN(TestEvidenceRuntime)
#include "tst_evidenceruntime.moc"
