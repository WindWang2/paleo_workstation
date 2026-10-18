#include "../src/ui/maptools/identifypopup.h"

#include <QLabel>
#include <QToolButton>
#include <QtTest>
#include <qgsapplication.h>

class TestIdentifyBrowse : public QObject
{
  Q_OBJECT
private slots:
  void switchesFloatingHits();
};

void TestIdentifyBrowse::switchesFloatingHits()
{
  IdentifyPopup popup;
  popup.setHits({
      {QStringLiteral("井位"), QStringLiteral("要素 1"),
       {{QStringLiteral("井名"), QStringLiteral("W-1")}, {QStringLiteral("砂地比"), QStringLiteral("0.4")}}},
      {QStringLiteral("等值线"), QStringLiteral("要素 8"),
       {{QStringLiteral("级别"), QStringLiteral("0.6")}}},
  });
  QCOMPARE(popup.count(), 2);
  QCOMPARE(popup.index(), 0);
  QCOMPARE(popup.findChild<QLabel *>(QStringLiteral("identifyTitle"))->text(), QStringLiteral("井位"));
  popup.findChild<QToolButton *>(QStringLiteral("identifyNext"))->click();
  QCOMPARE(popup.index(), 1);
  QCOMPARE(popup.findChild<QLabel *>(QStringLiteral("identifyTitle"))->text(), QStringLiteral("等值线"));
  QCOMPARE(popup.findChild<QLabel *>(QStringLiteral("identifyCounter"))->text(), QStringLiteral("2 / 2"));
  popup.findChild<QToolButton *>(QStringLiteral("identifyPrev"))->click();
  QCOMPARE(popup.index(), 0);
  popup.setHits({});
  QVERIFY(!popup.isVisible());
}

int main(int argc, char **argv)
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  TestIdentifyBrowse test;
  const int status = QTest::qExec(&test, argc, argv);
  QgsApplication::exitQgis();
  return status;
}

#include "tst_identifybrowse.moc"
