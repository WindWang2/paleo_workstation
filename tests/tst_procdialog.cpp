#include <QtTest>
#include <QDialog>
#include <QLabel>
#include <QPointer>
#include <QSettings>

#include <qgsapplication.h>
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingalgorithmwidgetbase.h>
#include <qgsprocessingparameterswidget.h>
#include <qgsprocessingregistry.h>

#include "../src/qgis/qgisprocessingservice.h"

// Acceptance for the native Processing algorithm-dialog wiring:
//   * the service ctor registers the PaleoProvider, so paleo:* ids resolve;
//   * showAlgorithmDialog() creates the real QgsProcessingAlgorithmWidgetBase
//     (hosted in a top-level dialog via QgsDockableWidgetHelper, forced to
//     non-docked dialog mode — offscreen safe, no exec() loop);
//   * the parameter panel is populated with wrapper-generated widgets;
//   * unknown algorithm ids fail with an error string.
class TestProcDialog : public QObject
{
  Q_OBJECT

private slots:

  void knownAlgorithmCreatesNativeDialog()
  {
    QgisProcessingService svc(nullptr); // ctor registers PaleoProvider

    QString error;
    QVERIFY2(svc.showAlgorithmDialog(
                 QStringLiteral("paleo:paleo_constraint_idw"),
                 QVariantMap{{QStringLiteral("CELL_SIZE"), 2.5}},
                 nullptr, &error),
             qPrintable(error));

    QWidget *w = svc.lastAlgorithmDialog();
    QVERIFY(w);
    auto *algWidget = qobject_cast<QgsProcessingAlgorithmWidgetBase *>(w);
    QVERIFY(algWidget);
    QVERIFY(algWidget->algorithm());
    QCOMPARE(algWidget->algorithm()->id(),
             QStringLiteral("paleo:paleo_constraint_idw"));

    // Parameter panel was built by the registry-backed
    // QgsProcessingParametersWidget port — it carries per-parameter widgets
    // (constraint_idw has INPUT/FIELD/CONSTRAINTS/FACIES_CODE/CELL_SIZE/OUTPUT).
    auto *panel =
        qobject_cast<QgsProcessingParametersWidget *>(algWidget->mainWidget());
    QVERIFY(panel);
    QVERIFY(panel->findChildren<QWidget *>().size() > 5);

    // The dockable helper hosts the widget inside a separate top-level QDialog.
    QVERIFY(w->window() != nullptr);
    QVERIFY(w->window()->isWindow());
    QVERIFY(qobject_cast<QDialog *>(w->window()));

    // Preset params flow through the panel's wrappers without errors:
    // createProcessingParameters() must report the CELL_SIZE override back.
    QCOMPARE(algWidget->createProcessingParameters()
                 .value(QStringLiteral("CELL_SIZE"))
                 .toDouble(),
             2.5);

    // Cleanup: close the hosting dialog and let deferred deletes flush.
    w->window()->close();
    w->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
  }

  void secondAlgorithmCreatesDialog()
  {
    QgisProcessingService svc(nullptr);

    QString error;
    QVERIFY2(svc.showAlgorithmDialog(
                 QStringLiteral("paleo:paleo_geological_smoothing"),
                 QVariantMap{{QStringLiteral("PASSES"), 2}}, nullptr, &error),
             qPrintable(error));
    auto *w = svc.lastAlgorithmDialog();
    QVERIFY(w);
    auto *algWidget = qobject_cast<QgsProcessingAlgorithmWidgetBase *>(w);
    QVERIFY(algWidget->algorithm());
    QCOMPARE(algWidget->algorithm()->id(),
             QStringLiteral("paleo:paleo_geological_smoothing"));
    QCOMPARE(algWidget->createProcessingParameters()
                 .value(QStringLiteral("PASSES"))
                 .toInt(),
             2);

    w->window()->close();
    w->deleteLater();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
  }

  void unknownAlgorithmFails()
  {
    QgisProcessingService svc(nullptr);

    QString error;
    QVERIFY(!svc.showAlgorithmDialog(QStringLiteral("paleo:does_not_exist"),
                                     QVariantMap(), nullptr, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(error.contains(QStringLiteral("paleo:does_not_exist")));
    QVERIFY(svc.lastAlgorithmDialog() == nullptr);
  }

  void nonPaleoAlgorithmFails()
  {
    QgisProcessingService svc(nullptr);
    QString error;
    QVERIFY(!svc.showAlgorithmDialog(QStringLiteral("qgis:buffer"), QVariantMap(),
                                     nullptr, &error));
    QVERIFY(!error.isEmpty());
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");

  // Keep QSettings writes out of the real user profile.
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     QDir::temp().filePath(QStringLiteral("paleo_tst_procdialog")));

  QgsApplication app(argc, argv, true); // GUI-enabled: dialog widgets required
  app.setPrefixPath(QStringLiteral("/usr"), true); // distro install
  app.initQgis();

  TestProcDialog tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QCoreApplication::processEvents(); // flush deferred dialog/widget deletes
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_procdialog.moc"
