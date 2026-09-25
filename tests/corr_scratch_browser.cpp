#include <QtTest>
#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "../src/io/lasparser.h"
#include "../src/ui/correlation/curvebrowser.h"

// Subtask B scratch test — CurveBrowser (LAS curve browser panel, §42.x).
// Contract under test:
//   - setCurves lists every mnemonic in listing order (unit/descr columns)
//   - check = add track, uncheck = remove track; user click and setChecked()
//     share one path -> mnemonicToggled(wellId, mnemonic, on)
//   - setChecked is idempotent (no re-emit for same state); unknown mnemonic
//     is a silent no-op
//   - relist keeps check state of surviving mnemonics and emits nothing
//   - §42.4 empty state guidance; well identity caption
class TestCurveBrowser : public QObject
{
  Q_OBJECT

  private:
    static QList<LasCurve> w1Curves()
    {
      return {{QStringLiteral("DEPT"), QStringLiteral("M"), QStringLiteral("深度"), {}},
              {QStringLiteral("GR"), QStringLiteral("GAPI"), QStringLiteral("伽马射线"), {}},
              {QStringLiteral("RHOB"), QStringLiteral("G/CM3"), QStringLiteral("体积密度"), {}},
              {QStringLiteral("NPHI"), QStringLiteral("V/V"), QStringLiteral("中子孔隙度"), {}}};
    }

    static QString wellIdOf(const QSignalSpy &spy, int i)
    {
      return spy.at(i).at(0).toString();
    }

  private slots:
    void initialEmptyStateShowsGuidance()
    {
      CurveBrowser browser(nullptr);
      QVERIFY(!browser.hasCurves());
      QVERIFY(browser.wellId().isEmpty());
      QVERIFY(browser.mnemonics().isEmpty());
      QVERIFY(browser.checkedMnemonics().isEmpty());

      browser.show();
      QVERIFY(QTest::qWaitForWindowExposed(&browser));

      // §42.4: empty panel shows warm guidance, never a blank view.
      auto *label = browser.findChild<QLabel *>(QStringLiteral("browserEmptyLabel"));
      QVERIFY(label);
      QVERIFY(label->isVisible());
      QCOMPARE(label->text(), QStringLiteral("导入 LAS 后选择曲线"));
    }

    void objectNamesForFindChild()
    {
      CurveBrowser browser(nullptr);
      QCOMPARE(browser.objectName(), QStringLiteral("curveBrowser"));
      QVERIFY(browser.findChild<QTreeWidget *>(QStringLiteral("curveList")));
      QVERIFY(browser.findChild<QLabel *>(QStringLiteral("wellLabel")));
    }

    void setCurvesListsAllMnemonicsInOrder()
    {
      CurveBrowser browser(nullptr);
      browser.show();
      QVERIFY(QTest::qWaitForWindowExposed(&browser));

      QSignalSpy spy(&browser, &CurveBrowser::mnemonicToggled);
      browser.setCurves(QStringLiteral("W1"), w1Curves());
      QCOMPARE(spy.count(), 0); // listing must stay silent

      QVERIFY(browser.hasCurves());
      QCOMPARE(browser.wellId(), QStringLiteral("W1"));
      QCOMPARE(browser.mnemonics(),
               QStringList({QStringLiteral("DEPT"), QStringLiteral("GR"),
                            QStringLiteral("RHOB"), QStringLiteral("NPHI")}));

      // Rows carry mnemonic + unit + description, DEPT included.
      auto *list = browser.findChild<QTreeWidget *>(QStringLiteral("curveList"));
      QVERIFY(list);
      QCOMPARE(list->topLevelItemCount(), 4);
      QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("DEPT"));
      QCOMPARE(list->topLevelItem(0)->text(1), QStringLiteral("M"));
      QCOMPARE(list->topLevelItem(1)->text(1), QStringLiteral("GAPI"));
      QCOMPARE(list->topLevelItem(2)->text(2), QStringLiteral("体积密度"));
      QCOMPARE(list->topLevelItem(3)->text(0), QStringLiteral("NPHI"));

      // Curves arrived -> guidance disappears.
      auto *label = browser.findChild<QLabel *>(QStringLiteral("browserEmptyLabel"));
      QVERIFY(label);
      QVERIFY(!label->isVisible());
    }

    void wellCaptionShowsCurrentWell()
    {
      CurveBrowser browser(nullptr);
      auto *caption = browser.findChild<QLabel *>(QStringLiteral("wellLabel"));
      QVERIFY(caption);
      QVERIFY(!caption->isVisible()); // no well yet -> hidden

      browser.setCurves(QStringLiteral("W1"), w1Curves());
      browser.show();
      QVERIFY(QTest::qWaitForWindowExposed(&browser));
      QVERIFY(caption->isVisible());
      QCOMPARE(caption->text(), QStringLiteral("井：W1"));

      // Still names the well when the listing empties again.
      browser.setCurves(QStringLiteral("W1"), {});
      QCOMPARE(caption->text(), QStringLiteral("井：W1"));
    }

    void setCheckedTrueEmitsAndIsIdempotent()
    {
      CurveBrowser browser(nullptr);
      browser.setCurves(QStringLiteral("W1"), w1Curves());

      QSignalSpy spy(&browser, &CurveBrowser::mnemonicToggled);
      browser.setChecked(QStringLiteral("GR"), true);
      QCOMPARE(spy.count(), 1);
      QCOMPARE(wellIdOf(spy, 0), QStringLiteral("W1"));
      QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("GR"));
      QCOMPARE(spy.at(0).at(2).toBool(), true);

      QVERIFY(browser.isChecked(QStringLiteral("GR")));
      QCOMPARE(browser.checkedMnemonics(), QStringList{QStringLiteral("GR")});

      // Same-state repeat: no re-announce (checks stay idempotent).
      browser.setChecked(QStringLiteral("GR"), true);
      QCOMPARE(spy.count(), 1);
    }

    void multipleChecksCoexist()
    {
      CurveBrowser browser(nullptr);
      browser.setCurves(QStringLiteral("W1"), w1Curves());

      QSignalSpy spy(&browser, &CurveBrowser::mnemonicToggled);
      browser.setChecked(QStringLiteral("DEPT"), true);
      browser.setChecked(QStringLiteral("GR"), true);
      browser.setChecked(QStringLiteral("NPHI"), true);
      QCOMPARE(spy.count(), 3);
      QCOMPARE(browser.checkedMnemonics(),
               QStringList({QStringLiteral("DEPT"), QStringLiteral("GR"),
                            QStringLiteral("NPHI")}));
      QVERIFY(browser.isChecked(QStringLiteral("DEPT")));
      QVERIFY(browser.isChecked(QStringLiteral("GR")));
      QVERIFY(!browser.isChecked(QStringLiteral("RHOB")));
    }

    void setCheckedFalseEmitsOff()
    {
      CurveBrowser browser(nullptr);
      browser.setCurves(QStringLiteral("W1"), w1Curves());
      browser.setChecked(QStringLiteral("GR"), true);

      QSignalSpy spy(&browser, &CurveBrowser::mnemonicToggled);
      browser.setChecked(QStringLiteral("GR"), false);
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("GR"));
      QCOMPARE(spy.at(0).at(2).toBool(), false);
      QVERIFY(!browser.isChecked(QStringLiteral("GR")));
      QVERIFY(browser.checkedMnemonics().isEmpty());

      // Idempotent off as well.
      browser.setChecked(QStringLiteral("GR"), false);
      QCOMPARE(spy.count(), 1);
    }

    void userClickEmitsLikeSetChecked()
    {
      CurveBrowser browser(nullptr);
      browser.setCurves(QStringLiteral("W1"), w1Curves());
      browser.resize(360, 320);
      browser.show();
      QVERIFY(QTest::qWaitForWindowExposed(&browser));

      auto *list = browser.findChild<QTreeWidget *>(QStringLiteral("curveList"));
      QVERIFY(list);
      auto *row = list->topLevelItem(1); // GR
      QVERIFY(row);

      // Click the row's check indicator (leading edge of an un-indented row).
      const QRect r = list->visualItemRect(row);
      const QPoint at(r.x() + 10, r.center().y());

      QSignalSpy spy(&browser, &CurveBrowser::mnemonicToggled);
      QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, at);
      QCOMPARE(spy.count(), 1);
      QCOMPARE(wellIdOf(spy, 0), QStringLiteral("W1"));
      QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("GR"));
      QCOMPARE(spy.at(0).at(2).toBool(), true);
      QVERIFY(browser.isChecked(QStringLiteral("GR")));

      QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, at);
      QCOMPARE(spy.count(), 2);
      QCOMPARE(spy.at(1).at(2).toBool(), false);
      QVERIFY(!browser.isChecked(QStringLiteral("GR")));
    }

    void relistKeepsChecksAndStaysSilent()
    {
      CurveBrowser browser(nullptr);
      browser.setCurves(QStringLiteral("W1"), w1Curves());
      browser.setChecked(QStringLiteral("GR"), true);
      browser.setChecked(QStringLiteral("RHOB"), true);

      // Same well, GR survives, RHOB dropped, one new curve arrives.
      QList<LasCurve> next = w1Curves();
      next.removeAt(2); // RHOB
      next.append({QStringLiteral("DT"), QStringLiteral("US/M"), QStringLiteral("声波"), {}});

      QSignalSpy spy(&browser, &CurveBrowser::mnemonicToggled);
      browser.setCurves(QStringLiteral("W1"), next);
      QCOMPARE(spy.count(), 0); // no toggles from a relist, not even for RHOB

      QVERIFY(browser.isChecked(QStringLiteral("GR")));
      QVERIFY(!browser.isChecked(QStringLiteral("RHOB")));
      QCOMPARE(browser.mnemonics().last(), QStringLiteral("DT"));
      QCOMPARE(browser.checkedMnemonics(), QStringList{QStringLiteral("GR")});

      // A later programmatic uncheck of the survivor still announces.
      browser.setChecked(QStringLiteral("GR"), false);
      QCOMPARE(spy.count(), 1);
      QCOMPARE(spy.at(0).at(2).toBool(), false);
    }

    void checkedStateCarriesAcrossWellSwitch()
    {
      CurveBrowser browser(nullptr);
      browser.setCurves(QStringLiteral("W1"), w1Curves());
      browser.setChecked(QStringLiteral("GR"), true);

      QList<LasCurve> other = w1Curves();
      other.removeAt(0); // drop DEPT, keep GR
      QSignalSpy spy(&browser, &CurveBrowser::mnemonicToggled);
      browser.setCurves(QStringLiteral("W2"), other);
      QCOMPARE(spy.count(), 0);
      QCOMPARE(browser.wellId(), QStringLiteral("W2"));
      QVERIFY(browser.isChecked(QStringLiteral("GR")));

      // Toggles after the switch name the new well.
      browser.setChecked(QStringLiteral("GR"), false);
      QCOMPARE(spy.count(), 1);
      QCOMPARE(wellIdOf(spy, 0), QStringLiteral("W2"));
    }

    void unknownMnemonicIsSilentNoOp()
    {
      CurveBrowser browser(nullptr);
      browser.setCurves(QStringLiteral("W1"), w1Curves());

      QSignalSpy spy(&browser, &CurveBrowser::mnemonicToggled);
      browser.setChecked(QStringLiteral("NOSUCH"), true);
      browser.setChecked(QStringLiteral("NOSUCH"), false);
      QCOMPARE(spy.count(), 0);
      QVERIFY(!browser.isChecked(QStringLiteral("NOSUCH")));
      QVERIFY(browser.checkedMnemonics().isEmpty());

      // Also on an empty browser.
      browser.setCurves(QStringLiteral("W1"), {});
      browser.setChecked(QStringLiteral("GR"), true);
      QCOMPARE(spy.count(), 0);
    }

    void clearingCurvesRestoresEmptyState()
    {
      CurveBrowser browser(nullptr);
      browser.setCurves(QStringLiteral("W1"), w1Curves());
      browser.setChecked(QStringLiteral("GR"), true);
      QVERIFY(browser.hasCurves());

      QSignalSpy spy(&browser, &CurveBrowser::mnemonicToggled);
      browser.setCurves(QStringLiteral("W1"), {});
      QCOMPARE(spy.count(), 0);
      QVERIFY(!browser.hasCurves());
      QVERIFY(browser.mnemonics().isEmpty());
      QVERIFY(browser.checkedMnemonics().isEmpty());

      browser.show();
      QVERIFY(QTest::qWaitForWindowExposed(&browser));
      auto *label = browser.findChild<QLabel *>(QStringLiteral("browserEmptyLabel"));
      QVERIFY(label);
      QVERIFY(label->isVisible());
      QCOMPARE(label->text(), QStringLiteral("导入 LAS 后选择曲线"));
    }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv); // widgets — QCoreApplication is not enough
  TestCurveBrowser tc;
  return QTest::qExec(&tc, argc, argv);
}

#include "corr_scratch_browser.moc"
