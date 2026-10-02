// 层：视图（测试壳位于 tests/，被测对象为视图层参数面板；offscreen 沙箱）
#include <QtTest>
#include <QtWidgets>

#include "ui/correlation/petrophyspanel.h"

#include <cmath>

using paleo::petrophys::PetroPhysPanel;
using PetroPhysTaskService = paleo::petrophys::PetroPhysTaskService;
using Formula = PetroPhysTaskService::Formula;

// 面板契约：视图只发信号不干活——表单值 → currentRequest() 映射、
// computeRequested 意图（wells 恒空，编排方填）、busy/progress/result
// 回填槽与使能门控。文献预填值（ρma 2.65 等）是显式参数形态，断言钉住
// 防漂移；Rw 0 → NaN（「必填」不臆造）。
class TestPetroPhysPanel : public QObject
{
  Q_OBJECT
private slots:
  void testBuildAndDefaults();
  void testKindSyncsOutputMnemonic();
  void testCurrentRequestMapsForm();
  void testComputeIntentSignal();
  void testBusyGating();
  void testProgressAndScopeBackfill();
};

void TestPetroPhysPanel::testBuildAndDefaults()
{
  PetroPhysPanel p;
  p.show();
  QVERIFY(p.findChild<QComboBox *>(QStringLiteral("petrophysKindCombo")));
  QVERIFY(p.findChild<QToolButton *>(QStringLiteral("petrophysComputeButton")));
  QVERIFY(p.findChild<QToolButton *>(QStringLiteral("petrophysCancelButton")));
  QVERIFY(p.findChild<QProgressBar *>(QStringLiteral("petrophysProgress")));
  QVERIFY(p.findChild<QLabel *>(QStringLiteral("petrophysStatus")));
  // 缺省：Vsh 线性 + 井内极值基线 + RW 必填位
  QCOMPARE(p.currentRequest().formula, Formula::VshGrLinear);
  QVERIFY(p.currentRequest().params.grAutoBaseline);
  QVERIFY(std::isnan(p.currentRequest().params.rw));
  QCOMPARE(p.currentOutputMnemonic(), QStringLiteral("VSH"));
}

void TestPetroPhysPanel::testKindSyncsOutputMnemonic()
{
  PetroPhysPanel p;
  auto *cbo = p.findChild<QComboBox *>(QStringLiteral("petrophysKindCombo"));
  const QList<QPair<Formula, QString>> want = {
      {Formula::PhiDensity, QStringLiteral("PHID")},
      {Formula::PhiNeutron, QStringLiteral("PHIN")},
      {Formula::PhiSonicWyllie, QStringLiteral("PHIS")},
      {Formula::SwArchie, QStringLiteral("SW")},
      {Formula::Expression, QStringLiteral("EXPR")}};
  for (const auto &w : want)
  {
    const int idx = cbo->findData(int(w.first));
    QVERIFY2(idx >= 0, "kind in combo");
    cbo->setCurrentIndex(idx);
    QCOMPARE(p.currentOutputMnemonic(), w.second);
  }
}

void TestPetroPhysPanel::testCurrentRequestMapsForm()
{
  PetroPhysPanel p;
  auto *chkAuto = p.findChild<QCheckBox *>(QStringLiteral("petrophysGrAuto"));
  auto *spinMin = p.findChild<QDoubleSpinBox *>(QStringLiteral("petrophysGrMin"));
  auto *spinMax = p.findChild<QDoubleSpinBox *>(QStringLiteral("petrophysGrMax"));
  auto *rw = p.findChild<QDoubleSpinBox *>(QStringLiteral("petrophysArchieRw"));

  chkAuto->setChecked(false);
  spinMin->setValue(10.0);
  spinMax->setValue(90.0);
  const auto r1 = p.currentRequest();
  QVERIFY(!r1.params.grAutoBaseline);
  QCOMPARE(r1.params.grMin, 10.0);
  QCOMPARE(r1.params.grMax, 90.0);

  // 文献预填值钉住（显式参数形态，出处 tooltip；漂移即红）
  const auto r2 = p.currentRequest();
  QCOMPARE(r2.params.rhoMa, 2.65);   // 石英砂岩（AK04 ch.3）
  QCOMPARE(r2.params.rhoFluid, 1.0); // 淡水
  QCOMPARE(r2.params.dtMa, 182.0);   // µs/m
  QCOMPARE(r2.params.dtFluid, 620.0);
  QCOMPARE(r2.params.cpFactor, 1.0);
  QCOMPARE(r2.params.archieA, 1.0);
  QCOMPARE(r2.params.archieM, 2.0);
  QCOMPARE(r2.params.archieN, 2.0);

  rw->setValue(0.05);
  QVERIFY(std::fabs(p.currentRequest().params.rw - 0.05) < 1e-12);

  // 表达式 + 输出名 + QC 门
  auto *expr = p.findChild<QLineEdit *>(QStringLiteral("petrophysExprEdit"));
  auto *out = p.findChild<QLineEdit *>(QStringLiteral("petrophysOutMnemonic"));
  auto *cbo = p.findChild<QComboBox *>(QStringLiteral("petrophysKindCombo"));
  cbo->setCurrentIndex(cbo->findData(int(Formula::Expression)));
  expr->setText(QStringLiteral("RHOB - 0.05*NPHI"));
  out->setText(QStringLiteral("EXR"));
  const auto r3 = p.currentRequest();
  QCOMPARE(r3.formula, Formula::Expression);
  QCOMPARE(r3.expression, QStringLiteral("RHOB - 0.05*NPHI"));
  QCOMPARE(r3.outputMnemonic, QStringLiteral("EXR"));
  QVERIFY(r3.qcBandEnabled);
  QCOMPARE(r3.qcLo, 0.0);
  QCOMPARE(r3.qcHi, 1.0);
}

void TestPetroPhysPanel::testComputeIntentSignal()
{
  PetroPhysPanel p;
  bool got = false;
  PetroPhysTaskService::BatchRequest captured;
  QObject::connect(&p, &PetroPhysPanel::computeRequested,
                   [&](const PetroPhysTaskService::BatchRequest &r)
                   {
                     got = true;
                     captured = r;
                   });
  p.findChild<QToolButton *>(QStringLiteral("petrophysComputeButton"))->click();
  QVERIFY(got);
  QVERIFY(captured.wells.isEmpty()); // 井集编排方填——视图不管
  QCOMPARE(captured.formula, Formula::VshGrLinear);
}

void TestPetroPhysPanel::testBusyGating()
{
  PetroPhysPanel p;
  auto *compute = p.findChild<QToolButton *>(QStringLiteral("petrophysComputeButton"));
  auto *cancel = p.findChild<QToolButton *>(QStringLiteral("petrophysCancelButton"));
  QVERIFY(compute->isEnabled());
  QVERIFY(!cancel->isEnabled());
  p.setBusy(true);
  QVERIFY(!compute->isEnabled());
  QVERIFY(cancel->isEnabled());
  p.showResult(false, QStringLiteral("失败原因"));
  QVERIFY(compute->isEnabled());
  QVERIFY(!cancel->isEnabled());
  QCOMPARE(p.findChild<QLabel *>(QStringLiteral("petrophysStatus"))->text(),
           QStringLiteral("失败原因"));
}

void TestPetroPhysPanel::testProgressAndScopeBackfill()
{
  PetroPhysPanel p;
  p.updateProgress(42, QStringLiteral("parse"));
  QCOMPARE(p.findChild<QProgressBar *>(QStringLiteral("petrophysProgress"))->value(), 42);
  p.setWellScope(5, 3);
  const QString lbl =
      p.findChild<QLabel *>(QStringLiteral("petrophysWellsLabel"))->text();
  QVERIFY2(lbl.contains(QStringLiteral("5")) && lbl.contains(QStringLiteral("3")),
           qPrintable(lbl));
  p.showResult(true, QStringLiteral("3/3 井完成"));
  QCOMPARE(p.findChild<QProgressBar *>(QStringLiteral("petrophysProgress"))->value(), 100);
}

QTEST_MAIN(TestPetroPhysPanel)
#include "tst_petrophyspanel.moc"
