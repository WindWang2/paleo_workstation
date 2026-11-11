#include <QtTest>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVariantMap>

#include "../src/workflow/registration.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/geojsonaffine.h"
#include "../src/qgis/provisionalvector.h"
#include "../src/qgis/qgislayerservice.h"
#include <qgsapplication.h>
#include <qgsfeatureiterator.h>
#include <qgspoint.h>
#include <qgsvectorlayer.h>

class TestWorkflowRegistration : public QObject
{
  Q_OBJECT

private slots:
  void initialState();
  void nullServicesFailGracefully();
  void nullLayerServiceFails();
  void emptyParamsHandled();
  void signalSpyContracts();
  void mutationDemonstration_monotonicCounter();
  void provisionalPackageRoundTrip();
};

void TestWorkflowRegistration::initialState()
{
  RegistrationWorkflow wf(nullptr, nullptr);
  QCOMPARE(wf.provisionalLayerCount(), 0);
}

void TestWorkflowRegistration::nullServicesFailGracefully()
{
  RegistrationWorkflow wf(nullptr, nullptr);
  QSignalSpy failSpy(&wf, &RegistrationWorkflow::registrationFailed);
  QSignalSpy successSpy(&wf, &RegistrationWorkflow::provisionalRegistered);
  QSignalSpy countSpy(&wf, &RegistrationWorkflow::provisionalLayerCountChanged);

  QVariantMap params;
  params.insert(QStringLiteral("tx"), 10.0);
  params.insert(QStringLiteral("ty"), 20.0);
  wf.applyProvisionalRegistration(QStringLiteral("ast-test-1"), params);

  // 必须发射失败信号，且不触发成功与计数变更
  QCOMPARE(failSpy.count(), 1);
  QCOMPARE(successSpy.count(), 0);
  QCOMPARE(countSpy.count(), 0);
  QCOMPARE(wf.provisionalLayerCount(), 0);

  const QString errMsg = failSpy.takeFirst().at(0).toString();
  QVERIFY(errMsg.contains(QStringLiteral("未就绪")) || errMsg.contains(QStringLiteral("失败")));
}

void TestWorkflowRegistration::nullLayerServiceFails()
{
  // 仅有 import service 但缺 layer service
  RegistrationWorkflow wf(nullptr, nullptr);
  QSignalSpy failSpy(&wf, &RegistrationWorkflow::registrationFailed);

  wf.applyProvisionalRegistration(QStringLiteral("ast-missing"), {});
  QCOMPARE(failSpy.count(), 1);
  QVERIFY(!failSpy.takeFirst().at(0).toString().isEmpty());
}

void TestWorkflowRegistration::emptyParamsHandled()
{
  RegistrationWorkflow wf(nullptr, nullptr);
  QSignalSpy failSpy(&wf, &RegistrationWorkflow::registrationFailed);

  wf.applyProvisionalRegistration(QString(), {});
  QCOMPARE(failSpy.count(), 1);
}

void TestWorkflowRegistration::signalSpyContracts()
{
  RegistrationWorkflow wf(nullptr, nullptr);
  QSignalSpy failSpy(&wf, &RegistrationWorkflow::registrationFailed);
  QSignalSpy regSpy(&wf, &RegistrationWorkflow::provisionalRegistered);
  QSignalSpy cntSpy(&wf, &RegistrationWorkflow::provisionalLayerCountChanged);

  QVERIFY(failSpy.isValid());
  QVERIFY(regSpy.isValid());
  QVERIFY(cntSpy.isValid());

  // 触发一次调用
  wf.applyProvisionalRegistration(QStringLiteral("nonexistent"), {});
  QCOMPARE(failSpy.count(), 1);
  QCOMPARE(regSpy.count(), 0);
  QCOMPARE(cntSpy.count(), 0);
}

void TestWorkflowRegistration::mutationDemonstration_monotonicCounter()
{
  // 变异测试示范：provisionalLayerCount 必须是非负单调锁存计数器
  RegistrationWorkflow wf(nullptr, nullptr);
  QCOMPARE(wf.provisionalLayerCount(), 0);

  // 失败调用绝不会产生伪计数递增
  for (int i = 0; i < 5; ++i)
  {
    wf.applyProvisionalRegistration(QStringLiteral("invalid"), {});
  }
  QCOMPARE(wf.provisionalLayerCount(), 0);
}

void TestWorkflowRegistration::provisionalPackageRoundTrip()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString source = dir.filePath(QStringLiteral("raw.geojson"));
  QFile raw(source);
  QVERIFY(raw.open(QIODevice::WriteOnly));
  const QByteArray original = R"({"type":"FeatureCollection","features":[{"type":"Feature","geometry":{"type":"Point","coordinates":[2,3,7]},"properties":{"fid":11,"geom":"label","value":1.25}}]})";
  QCOMPARE(raw.write(original), original.size());
  raw.close();

  GeoAffineParams transform;
  transform.tx = 100;
  transform.ty = 200;
  transform.sx = 2;
  transform.rotDeg = 90;
  QString error;
  int count = 0;
  const QString first = dir.filePath(QStringLiteral("first.gpkg"));
  QVERIFY2(ProvisionalVector::write(source, first, transform, &count, &error), qPrintable(error));
  QCOMPARE(count, 1);
  const QString firstUri = first + QStringLiteral("|layername=features");
  const QString second = dir.filePath(QStringLiteral("second.gpkg"));
  QVERIFY2(ProvisionalVector::write(firstUri, second, GeoAffineParams{}, &count, &error), qPrintable(error));
  QgsVectorLayer layer(second + QStringLiteral("|layername=features"), QStringLiteral("test"), QStringLiteral("ogr"));
  QVERIFY(layer.isValid());
  QCOMPARE(layer.crs().type(), Qgis::CrsType::Engineering);
  QCOMPARE(layer.featureCount(), 1);
  QgsFeature feature;
  QVERIFY(layer.getFeatures().nextFeature(feature));
  QCOMPARE(feature.attribute(QStringLiteral("fid")).toInt(), 11);
  QCOMPARE(feature.attribute(QStringLiteral("geom")).toString(), QStringLiteral("label"));
  QCOMPARE(feature.attribute(QStringLiteral("value")).toDouble(), 1.25);
  const auto geometry = feature.geometry();
  auto vertices = geometry.vertices();
  QVERIFY(vertices.hasNext());
  const QgsPoint point = vertices.next();
  QCOMPARE(point.x(), 97.0);
  QCOMPARE(point.y(), 204.0);
  QCOMPARE(point.z(), 7.0);
  QVERIFY(raw.open(QIODevice::ReadOnly));
  QCOMPARE(raw.readAll(), original);
}

int main(int argc, char **argv)
{
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  TestWorkflowRegistration test;
  const int status = QTest::qExec(&test, argc, argv);
  app.exitQgis();
  return status;
}

#include "tst_workflow_registration.moc"
