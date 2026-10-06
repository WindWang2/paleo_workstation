// 层：测试（数据层 io/geojsonaffine）
// BIZ-07（方向58）：临时配准仿射参数的有限性/量程闸。
// NaN/Inf/零缩放必须在写盘前被拒绝并给出原因；正常参数路径逐值不变。
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <limits>

#include "../src/io/geojsonaffine.h"

namespace
{
QString writeFixture(const QTemporaryDir &dir)
{
  const QString path = dir.filePath(QStringLiteral("src.geojson"));
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    return QString();
  f.write(R"({"type":"FeatureCollection","features":[
    {"type":"Feature","properties":{"n":1},"geometry":{"type":"Point","coordinates":[10.0,20.0]}},
    {"type":"Feature","properties":{"n":2},"geometry":{"type":"LineString","coordinates":[[0,0],[1,2]]}}]})");
  return path;
}
} // namespace

class TestGeoAffineGuard : public QObject
{
  Q_OBJECT
private slots:
  void rejectsNonFiniteAndZeroScale_data();
  void rejectsNonFiniteAndZeroScale();
  void validParamsUnchanged();
};

void TestGeoAffineGuard::rejectsNonFiniteAndZeroScale_data()
{
  QTest::addColumn<int>("field"); // 0 tx 1 ty 2 sx 3 sy 4 rot
  QTest::addColumn<double>("value");
  QTest::addColumn<QString>("needle");
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  QTest::newRow("tx=NaN") << 0 << nan << QStringLiteral("tx");
  QTest::newRow("ty=+Inf") << 1 << inf << QStringLiteral("ty");
  QTest::newRow("sx=NaN") << 2 << nan << QStringLiteral("sx");
  QTest::newRow("sy=-Inf") << 3 << -inf << QStringLiteral("sy");
  QTest::newRow("rot=NaN") << 4 << nan << QStringLiteral("rotDeg");
  QTest::newRow("sx=0") << 2 << 0.0 << QStringLiteral("sx");
  QTest::newRow("sy=0") << 3 << 0.0 << QStringLiteral("sy");
  QTest::newRow("sx=1e-12") << 2 << 1e-12 << QStringLiteral("sx");
  QTest::newRow("tx=1e300") << 0 << 1e300 << QStringLiteral("tx");
}

void TestGeoAffineGuard::rejectsNonFiniteAndZeroScale()
{
  QFETCH(int, field);
  QFETCH(double, value);
  QFETCH(QString, needle);
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString src = writeFixture(dir);
  QVERIFY(!src.isEmpty());
  GeoAffineParams p;
  double *fields[] = {&p.tx, &p.ty, &p.sx, &p.sy, &p.rotDeg};
  *fields[field] = value;

  QString why;
  QVERIFY(!geoAffineParamsValid(p, &why));
  QVERIFY2(why.contains(needle), qPrintable(why));

  // 文件级入口同样拒绝，且不留输出文件（写盘前拦截）。
  const QString out = dir.filePath(QStringLiteral("out.geojson"));
  QString err;
  QVERIFY(!geoAffineTransformFile(src, out, p, &err));
  QVERIFY2(err.contains(needle), qPrintable(err));
  QVERIFY(!QFile::exists(out));
}

void TestGeoAffineGuard::validParamsUnchanged()
{
  QTemporaryDir dir;
  QVERIFY(dir.isValid());
  const QString src = writeFixture(dir);
  GeoAffineParams p;
  p.tx = 500.0;
  p.ty = -250.0;
  p.sx = -2.0; // 镜像缩放是合法仿射（只拒零/近零）
  p.sy = 0.5;
  p.rotDeg = 30.0;
  QString why;
  QVERIFY2(geoAffineParamsValid(p, &why), qPrintable(why));

  const QString out = dir.filePath(QStringLiteral("out.geojson"));
  QString err;
  int n = 0;
  double b[4] = {0, 0, 0, 0};
  QVERIFY2(geoAffineTransformFile(src, out, p, &err, &n, b), qPrintable(err));
  QCOMPARE(n, 2);
  QFile f(out);
  QVERIFY(f.open(QIODevice::ReadOnly));
  const QJsonArray feats =
      QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("features")).toArray();
  const QJsonArray pt =
      feats.at(0).toObject().value(QStringLiteral("geometry")).toObject().value(
          QStringLiteral("coordinates")).toArray();
  double ex = 0, ey = 0;
  geoAffineApply(p, 10.0, 20.0, &ex, &ey);
  QCOMPARE(pt.at(0).toDouble(), ex);
  QCOMPARE(pt.at(1).toDouble(), ey);
  QVERIFY(std::isfinite(ex) && std::isfinite(ey));
}

QTEST_GUILESS_MAIN(TestGeoAffineGuard)
#include "tst_import_geoaffine.moc"
