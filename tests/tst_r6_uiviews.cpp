// R6 ui-views 回归：#837 #846 #847 #852 #853 #854（尺寸约束/滚轮缩放/预览曲线）。
#include <QtTest>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QScreen>
#include <QScrollArea>
#include <QSplitter>
#include <qgsapplication.h>
#include "ui/common/fitwidth.h"
#include "ui/common/screenfit.h"
#include "ui/common/wheelzoom.h"
#include "ui/crossplot/crossplotpanel.h"
#include "ui/datapreview/previewcurvepanel.h"
#include "ui/paleotheme.h"
#include "ui/seismicsection/seismicattrpanel.h"
#include "ui/seismicsection/seismicpickpanel.h"
#include "ui/seismicsection/inversionpanel.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"

using namespace paleo::crossplot;

namespace {
// 可选证据截图：R6_SHOT_DIR=<dir> 时保存控件快照（before/after 对比用）。
void shot(QWidget *w, const QString &name) {
  const QString dir = qEnvironmentVariable("R6_SHOT_DIR");
  if (dir.isEmpty())
    return;
  QCoreApplication::processEvents();
  w->grab().save(dir + QLatin1Char('/') + name + QStringLiteral(".png"));
}
} // namespace

class TestR6UiViews : public QObject {
  Q_OBJECT
private slots:
  void wheelZoomNormalisedByDelta();          // #852
  void fitWidthNeverClipsContent();           // #846/#847/#850
  void dialogDefaultBoundedToScreen();        // #854
  void crossplotControlsInResizableSplitter(); // #837
  void seismicPanelsDoNotForceDockWidth();    // #846
  void seismicCoordLabelDoesNotGrowMinWidth(); // #846
  void previewCurveSortedAndLasNullDropped(); // #853
};

void TestR6UiViews::wheelZoomNormalisedByDelta() {
  using paleo::ui::wheelZoomFactor;
  QVERIFY(qFuzzyCompare(wheelZoomFactor(120, 1.15), 1.15));
  QVERIFY(qFuzzyCompare(wheelZoomFactor(-120, 1.15), 1.0 / 1.15));
  // 触控板：8 个 15 单位事件 == 一个标准刻度（原实现 = 1.15^8）。
  double acc = 1.0;
  for (int i = 0; i < 8; ++i)
    acc *= wheelZoomFactor(15, 1.15);
  QVERIFY(qAbs(acc - 1.15) < 1e-9);
}

void TestR6UiViews::fitWidthNeverClipsContent() {
  QWidget host;
  auto *lay = new QHBoxLayout(&host);
  auto *sb = new QDoubleSpinBox(&host);
  sb->setRange(-100000.0, 100000.0);
  sb->setDecimals(3);
  paleo::ui::fitWidthAtLeast(sb, 48);
  lay->addWidget(sb);
  lay->addStretch(1);
  host.resize(600, 40);
  host.show();
  QVERIFY(QTest::qWaitForWindowExposed(&host));
  QVERIFY(sb->width() >= sb->sizeHint().width());
  QVERIFY(sb->width() >= 48);
}

void TestR6UiViews::dialogDefaultBoundedToScreen() {
  QScreen *scr = QGuiApplication::primaryScreen();
  QVERIFY(scr);
  const QSize avail = scr->availableGeometry().size();
  const QSize got = paleo::ui::boundedToScreen(avail * 2, scr);
  QVERIFY(got.width() <= avail.width() && got.height() <= avail.height());
  QCOMPARE(paleo::ui::boundedToScreen({10, 10}, scr), QSize(10, 10));
}

void TestR6UiViews::crossplotControlsInResizableSplitter() {
  PaleoTheme::pinRenderEnvironment();
  CrossplotPanel panel;
  panel.resize(1200, 600);
  panel.show();
  QVERIFY(QTest::qWaitForWindowExposed(&panel));
  auto *split = panel.findChild<QSplitter *>(QStringLiteral("crossplotSplitter"));
  panel.setSources({{"a", "A1井_GR_自然伽马_校正后_2026版本", "well"},
                    {"b", "A2井_RHOB_体积密度_环境校正", "well"}});
  panel.resize(760, 420);
  QCoreApplication::processEvents();
  shot(&panel, QStringLiteral("crossplot-760x420"));
  panel.resize(1200, 600);
  QCoreApplication::processEvents();
  QVERIFY(split); // 原实现：普通 QHBoxLayout + 固定 320
  split->setSizes({560, 600});
  QCoreApplication::processEvents();
  QVERIFY(split->sizes().at(0) > 320); // 控制栏可拉宽（长通道名）
  split->setSizes({0, 1160});
  QCoreApplication::processEvents();
  QCOMPARE(split->sizes().at(0), 0); // 可收起给图让位
}

void TestR6UiViews::seismicPanelsDoNotForceDockWidth() {
  seismic::SeismicSectionDockWidget dock;
  dock.resize(900, 650);
  dock.show();
  QVERIFY(QTest::qWaitForWindowExposed(&dock));
  dock.pickPanel()->setVisible(true);
  dock.attrPanel()->setVisible(true);
  dock.inversionPanel()->setVisible(true);
  QCoreApplication::processEvents();
  dock.resize(560, 520);
  QCoreApplication::processEvents();
  shot(&dock, QStringLiteral("seismicdock-panels-request560x520-got%1x%2")
                  .arg(dock.width()).arg(dock.height()));
  QVERIFY(dock.attrPanel()->isVisible());
  // 原实现：属性面板 row2 ≥745px 直接成为 dock 最小宽。
  QVERIFY2(dock.minimumSizeHint().width() < 500,
           qPrintable(QString::number(dock.minimumSizeHint().width())));
  QVERIFY(dock.minimumSizeHint().width() < dock.attrPanel()->minimumSizeHint().width());
  // 宿主行可见性跟随面板。
  QWidget *row = dock.attrPanel()->parentWidget();
  while (row && !qobject_cast<QScrollArea *>(row))
    row = row->parentWidget();
  QVERIFY(row);
  dock.attrPanel()->setVisible(false);
  QCoreApplication::processEvents();
  QVERIFY(row->isHidden());
  dock.attrPanel()->setVisible(true);
  QCoreApplication::processEvents();
  QVERIFY(!row->isHidden());
}

void TestR6UiViews::seismicCoordLabelDoesNotGrowMinWidth() {
  seismic::SeismicSectionDockWidget dock;
  dock.resize(700, 500);
  dock.show();
  QVERIFY(QTest::qWaitForWindowExposed(&dock));
  auto *lbl = dock.findChild<QLabel *>(QStringLiteral("lblSectionCoordinates"));
  QVERIFY(lbl);
  const int before = dock.minimumSizeHint().width();
  lbl->setText(QString(QStringLiteral("道: 123456 | 距离: 123456.7 m | TWT: 1234.5 ms | 深度: "
                                      "12345.6 m | 振幅: -12345.678 | (X: 12345678.9, Y: 1234567.8)"))
                   .repeated(2));
  QCoreApplication::processEvents();
  QCOMPARE(dock.minimumSizeHint().width(), before);
}

void TestR6UiViews::previewCurveSortedAndLasNullDropped() {
  paleo::datapreview_detail::CurvePanel panel;
  panel.addCurve(QStringLiteral("GR"), QStringLiteral("API"), {10, -999.25, 30, 40},
                 {400, 300, 200, 100}, Qt::red);
  const QVector<QPointF> pts = panel.curvePoints(QStringLiteral("GR"));
  QCOMPARE(pts.size(), 3);
  QCOMPARE(pts.first().y(), 100.0);
  QCOMPARE(pts.last().y(), 400.0);
  QCOMPARE(pts.first().x(), 40.0);
}

int main(int argc, char *argv[]) {
  QgsApplication app(argc, argv, true);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  TestR6UiViews t;
  const int rc = QTest::qExec(&t, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}
#include "tst_r6_uiviews.moc"
