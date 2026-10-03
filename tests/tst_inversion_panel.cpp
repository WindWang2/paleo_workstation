// 层：视图（测试壳位于 tests/，被测对象为视图层参数面板）
// goal/seismic-inversion — 反演面板：表单值 → 意图信号；busy/进度/结果回填。
#include <QtTest/QtTest>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QSignalSpy>
#include <QSpinBox>
#include <QToolButton>

#include "ui/seismicsection/inversionpanel.h"

using namespace seismic;

class TestInversionPanel : public QObject
{
    Q_OBJECT

private slots:
    void defaultsAndParams();
    void runEmitsInversionRequested();
    void busyGuardsRun();
    void extractAndBrowseSignals();
    void statusHonesty();
};

void TestInversionPanel::defaultsAndParams()
{
    InversionPanel panel;
    const InversionPanelParams p = panel.currentParams();
    QCOMPARE(p.method, QStringLiteral("bandlimited"));
    QCOMPARE(p.lowCutHz, 8.0);
    QCOMPARE(p.lambda, 0.0);
    QCOMPARE(p.maxIterations, 200);
    QVERIFY(p.waveletPath.isEmpty());
}

void TestInversionPanel::runEmitsInversionRequested()
{
    InversionPanel panel;
    auto *cbo = panel.findChild<QComboBox *>(QStringLiteral("invMethodCombo"));
    auto *lowCut = panel.findChild<QDoubleSpinBox *>(QStringLiteral("invLowCut"));
    auto *edit = panel.findChild<QLineEdit *>(QStringLiteral("invWaveletPath"));
    QVERIFY(cbo && lowCut && edit);
    cbo->setCurrentIndex(1); // 稀疏脉冲
    lowCut->setValue(10.0);
    edit->setText(QStringLiteral("/tmp/wavelet.json"));

    QSignalSpy spy(&panel, &InversionPanel::inversionRequested);
    auto *btn = panel.findChild<QToolButton *>(QStringLiteral("invRunButton"));
    QVERIFY(btn);
    btn->click();
    QCOMPARE(spy.count(), 1);
    const auto args = spy.takeFirst();
    const InversionPanelParams p = args.at(0).value<InversionPanelParams>();
    QCOMPARE(p.method, QStringLiteral("sparse"));
    QCOMPARE(p.lowCutHz, 10.0);
    QCOMPARE(p.waveletPath, QStringLiteral("/tmp/wavelet.json"));
}

void TestInversionPanel::busyGuardsRun()
{
    InversionPanel panel;
    QVERIFY(!panel.isBusy());
    panel.setBusy(true);
    QVERIFY(panel.isBusy());
    auto *btn = panel.findChild<QToolButton *>(QStringLiteral("invRunButton"));
    QVERIFY(btn);
    QVERIFY(!btn->isEnabled());
    QSignalSpy spy(&panel, &InversionPanel::inversionRequested);
    btn->click();
    QCOMPARE(spy.count(), 0); // busy 期不重复发起
    panel.showResult(true, QStringLiteral("done"));
    QVERIFY(!panel.isBusy());
    QVERIFY(btn->isEnabled());
}

void TestInversionPanel::extractAndBrowseSignals()
{
    InversionPanel panel;
    QSignalSpy spy(&panel, &InversionPanel::extractWaveletRequested);
    auto *btn = panel.findChild<QToolButton *>(QStringLiteral("invExtractWaveletButton"));
    QVERIFY(btn);
    btn->click();
    QCOMPARE(spy.count(), 1);

    QSignalSpy cancelSpy(&panel, &InversionPanel::cancelRequested);
    panel.setBusy(true);
    auto *cancel = panel.findChild<QToolButton *>(QStringLiteral("invCancelButton"));
    QVERIFY(cancel && cancel->isEnabled());
    cancel->click();
    QCOMPARE(cancelSpy.count(), 1);

    panel.setWaveletPath(QStringLiteral("/x/w.json"));
    auto *edit = panel.findChild<QLineEdit *>(QStringLiteral("invWaveletPath"));
    QCOMPARE(edit->text(), QStringLiteral("/x/w.json"));
}

void TestInversionPanel::statusHonesty()
{
    // 面板文案不标「高分辨率」——反演结果频段如实（低频+带限）。
    InversionPanel panel;
    panel.showResult(false, QStringLiteral("失败：缺子波"));
    auto *lbl = panel.findChild<QLabel *>(QStringLiteral("invStatusLabel"));
    QVERIFY(lbl);
    QVERIFY(!lbl->text().contains(QStringLiteral("高分辨率")));
    QVERIFY(panel.currentParams().lowCutHz > 0.0);
}

QTEST_MAIN(TestInversionPanel)
#include "tst_inversion_panel.moc"
