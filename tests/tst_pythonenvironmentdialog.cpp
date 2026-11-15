// 层：测试壳
#include "ui/ai/pythonenvironmentdialog.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QtTest>

class TestPythonEnvironmentDialog : public QObject {
  Q_OBJECT
private slots:
  void installedPackageOnlyUpdatesMatchingCandidateAndPreservesConsent() {
    PythonEnvironmentDialog dialog;
    dialog.setConfiguration({{"runtimeRoot", "/old"}, {"scriptsRoot", "/scripts"}, {"allowTrusted", true}});
    QSignalSpy applied(&dialog, &PythonEnvironmentDialog::activationRequested);
    dialog.setInstalledCandidate({{"kind", "runtime"}, {"root", "/new"}, {"manifestSha256", QString(64, 'a')}});
    QCOMPARE(dialog.findChild<QLineEdit *>("pythonRuntimeRoot")->text(), QString("/new"));
    QCOMPARE(dialog.findChild<QLineEdit *>("pythonScriptsRoot")->text(), QString("/scripts"));
    QVERIFY(dialog.findChild<QCheckBox *>("pythonAllowTrusted")->isChecked());
    QVERIFY(!dialog.findChild<QCheckBox *>("pythonAllowGenerated")->isChecked());
    QCOMPARE(applied.count(), 0);
  }
  void readinessDoesNotGrantExecutionConsent() {
    PythonEnvironmentDialog dialog;
    dialog.setDiagnostics({{"installed", "installed"}, {"integrity", "verified"},
                           {"trusted", "ready"}, {"generated", "ready"}});
    const auto trusted = dialog.findChild<QCheckBox *>("pythonAllowTrusted");
    const auto generated = dialog.findChild<QCheckBox *>("pythonAllowGenerated");
    QVERIFY(trusted); QVERIFY(generated); QVERIFY(!trusted->isChecked()); QVERIFY(!generated->isChecked());
    QSignalSpy applied(&dialog, &PythonEnvironmentDialog::activationRequested);
    dialog.findChild<QPushButton *>("pythonEnvironmentActivate")->click();
    QCOMPARE(applied.count(), 1);
    const auto form = applied.takeFirst().first().toJsonObject();
    QVERIFY(!form.value("allowTrusted").toBool()); QVERIFY(!form.value("allowGenerated").toBool());
  }
  void diagnosticRefreshPreservesUserDraftAndCompleteAnchor() {
    PythonEnvironmentDialog dialog;
    dialog.setConfiguration({{"runtimeRoot", "/old-root"}, {"allowTrusted", true}, {"allowGenerated", false}});
    auto *root = dialog.findChild<QLineEdit *>("pythonRuntimeRoot");
    auto *anchor = dialog.findChild<QLineEdit *>("pythonRuntimeManifestAnchor");
    QVERIFY(root); QVERIFY(anchor); root->setText("/new-draft");
    const auto malformedAnchor = QString(65, 'f'); anchor->setText(malformedAnchor);
    dialog.setDiagnostics({{"installed", "missing"}, {"generated", "unavailable"}});
    QCOMPARE(root->text(), QString("/new-draft")); QCOMPARE(anchor->text(), malformedAnchor);
    QSignalSpy applied(&dialog, &PythonEnvironmentDialog::activationRequested);
    dialog.findChild<QPushButton *>("pythonEnvironmentActivate")->click();
    QCOMPARE(applied.count(), 1); const auto form = applied.takeFirst().first().toJsonObject();
    QCOMPARE(form.value("runtimeRoot").toString(), QString("/new-draft"));
    QCOMPARE(form.value("runtimeManifestSha256").toString(), malformedAnchor);
    QVERIFY(form.value("allowTrusted").toBool()); QVERIFY(!form.value("allowGenerated").toBool());
  }
  void closeDuringTaskCancelsOnceAndWaitsForCompletion() {
    PythonEnvironmentDialog dialog; dialog.show(); dialog.setBusy(true);
    QSignalSpy cancelled(&dialog, &PythonEnvironmentDialog::cancelRequested);
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    QSignalSpy applied(&dialog, &PythonEnvironmentDialog::activationRequested);
    QSignalSpy disabled(&dialog, &PythonEnvironmentDialog::disableRequested);
    dialog.findChild<QPushButton *>("pythonEnvironmentDisable")->click();
    QCOMPARE(disabled.count(), 1);
    auto *buttons = dialog.findChild<QDialogButtonBox *>(); QVERIFY(buttons);
    buttons->button(QDialogButtonBox::Close)->click();
    buttons->button(QDialogButtonBox::Close)->click();
    dialog.findChild<QPushButton *>("pythonEnvironmentActivate")->click();
    QCOMPARE(cancelled.count(), 1); QCOMPARE(rejected.count(), 0); QCOMPARE(applied.count(), 0);
    QVERIFY(dialog.isVisible());
    dialog.setBusy(false); QCOMPARE(rejected.count(), 1); QVERIFY(!dialog.isVisible());
  }
};
QTEST_MAIN(TestPythonEnvironmentDialog)
#include "tst_pythonenvironmentdialog.moc"
