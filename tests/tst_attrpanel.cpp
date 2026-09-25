#include <QtTest>
#include <QComboBox>
#include <QLabel>
#include <QTemporaryDir>

#include <qgsmapcanvas.h>
#include <qgsvectorlayer.h>
#include <qgsattributetableview.h>
#include <qgsfeature.h>
#include <qgsgeometry.h>
#include <qgspointxy.h>

#include "../src/ui/attributetablepanel.h"
#include "../src/qgis/qgisruntime.h"

// AttributeTablePanel: provider-resolved vector layer → native
// QgsAttributeTableView chain (cache + model + filter) shows rows.
class TestAttrPanel : public QObject
{
  Q_OBJECT
private slots:

  void showsLayerRows()
  {
    QgsMapCanvas canvas;
    auto *vl = new QgsVectorLayer(
        QStringLiteral("Point?crs=EPSG:4326&field=name:string&field=z:double"),
        QStringLiteral("wells"), QStringLiteral("memory"));
    QVERIFY(vl->isValid());
    QgsFeature f(vl->fields());
    f.setGeometry(QgsGeometry::fromPointXY(QgsPointXY(1, 2)));
    f.setAttribute(QStringLiteral("name"), QStringLiteral("W1"));
    f.setAttribute(QStringLiteral("z"), 12.5);
    QList<QgsFeature> feats{f};
    QVERIFY(vl->dataProvider()->addFeatures(feats));
    vl->updateExtents();

    AttributeTablePanel panel(
        &canvas, [vl](const QString &id) -> QgsVectorLayer * {
          return id == QLatin1String("wells") ? vl : nullptr;
        });
    panel.setLayerIds({QStringLiteral("wells"), QStringLiteral("constraints.T1")});
    panel.showLayer(QStringLiteral("wells"));

    QCOMPARE(panel.currentLayerId(), QStringLiteral("wells"));
    auto *view = panel.findChild<QgsAttributeTableView *>(QStringLiteral("attrView"));
    QVERIFY(view->model());
    QCOMPARE(view->model()->rowCount(), 1);

    // Unknown layer id → hint visible, model cleared.
    panel.showLayer(QStringLiteral("nope"));
    QVERIFY(panel.findChild<QLabel *>(QStringLiteral("attrEmptyHint"))->isVisibleTo(&panel));
    delete vl;
  }

  void nullProviderSafe()
  {
    QgsMapCanvas canvas;
    AttributeTablePanel panel(&canvas, {});
    panel.showLayer(QStringLiteral("x")); // must not crash
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
    qFatal("QgisRuntime::initialize failed");
  TestAttrPanel tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_attrpanel.moc"
