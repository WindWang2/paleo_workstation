#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <limits>

#include "../src/io/laswriter.h"

class TestIoLasWriter : public QObject
{
  Q_OBJECT

private slots:
  void emptyCurvesFails();
  void mismatchedLengthsFails();
  void validLasFileGeneration();
  void nullTokenSubstitutionForNaN();
  void customOptionsPrecision();
  void mutationDemonstration_headerStructure();
};

void TestIoLasWriter::emptyCurvesFails()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString out = QDir(tmp.path()).filePath(QStringLiteral("empty.las"));

  QString err;
  const bool ok = LasWriter::writeLasFile(out, {}, {}, &err);
  QVERIFY(!ok);
  QVERIFY(!err.isEmpty());
}

void TestIoLasWriter::mismatchedLengthsFails()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString out = QDir(tmp.path()).filePath(QStringLiteral("mismatch.las"));

  LasCurve dept;
  dept.name = QStringLiteral("DEPT");
  dept.unit = QStringLiteral("M");
  dept.values = {100.0, 100.5, 101.0};

  LasCurve gr;
  gr.name = QStringLiteral("GR");
  gr.unit = QStringLiteral("GAPI");
  gr.values = {50.0, 55.0}; // 仅 2 个点，与 DEPT 不等长

  QString err;
  const bool ok = LasWriter::writeLasFile(out, {dept, gr}, {}, &err);
  QVERIFY(!ok);
  QVERIFY(err.contains(QStringLiteral("length")));
}

void TestIoLasWriter::validLasFileGeneration()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString out = QDir(tmp.path()).filePath(QStringLiteral("valid.las"));

  LasCurve dept;
  dept.name = QStringLiteral("DEPT");
  dept.unit = QStringLiteral("M");
  dept.values = {100.0, 100.5, 101.0};

  LasCurve gr;
  gr.name = QStringLiteral("GR");
  gr.unit = QStringLiteral("GAPI");
  gr.values = {45.2, 52.8, 60.1};

  LasWriteOptions opt;
  opt.wellName = QStringLiteral("WELL-001");

  QString err;
  const bool ok = LasWriter::writeLasFile(out, {dept, gr}, opt, &err);
  QVERIFY2(ok, qPrintable(err));
  QVERIFY(QFile::exists(out));

  QFile f(out);
  QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
  const QString content = QString::fromUtf8(f.readAll());

  QVERIFY(content.contains(QStringLiteral("~V")));
  QVERIFY(content.contains(QStringLiteral("~W")));
  QVERIFY(content.contains(QStringLiteral("~C")));
  QVERIFY(content.contains(QStringLiteral("~A")));
  QVERIFY(content.contains(QStringLiteral("WELL-001")));
  QVERIFY(content.contains(QStringLiteral("DEPT")));
  QVERIFY(content.contains(QStringLiteral("GR")));
}

void TestIoLasWriter::nullTokenSubstitutionForNaN()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString out = QDir(tmp.path()).filePath(QStringLiteral("nan_token.las"));

  LasCurve dept;
  dept.name = QStringLiteral("DEPT");
  dept.values = {100.0, 100.5};

  LasCurve res;
  res.name = QStringLiteral("RT");
  res.values = {10.5, std::numeric_limits<double>::quiet_NaN()};

  LasWriteOptions opt;
  opt.nullToken = -999.25;

  QString err;
  const bool ok = LasWriter::writeLasFile(out, {dept, res}, opt, &err);
  QVERIFY2(ok, qPrintable(err));

  QFile f(out);
  QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
  const QString content = QString::fromUtf8(f.readAll());
  QVERIFY(content.contains(QStringLiteral("-999.25")));
}

void TestIoLasWriter::customOptionsPrecision()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString out = QDir(tmp.path()).filePath(QStringLiteral("custom.las"));

  LasCurve dept;
  dept.name = QStringLiteral("DEPT");
  dept.values = {100.123456};

  LasCurve rho;
  rho.name = QStringLiteral("RHOB");
  rho.values = {2.456789};

  LasWriteOptions opt;
  opt.valuePrecision = 2;
  opt.nullToken = -9999.0;

  QString err;
  const bool ok = LasWriter::writeLasFile(out, {dept, rho}, opt, &err);
  QVERIFY2(ok, qPrintable(err));

  QFile f(out);
  QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
  const QString content = QString::fromUtf8(f.readAll());
  QVERIFY(content.contains(QStringLiteral("2.46"))); // 四舍五入到两位
}

void TestIoLasWriter::mutationDemonstration_headerStructure()
{
  // 变异测试示范：LAS 文件必须具有规范的 ~A 数据段标记，且首列为深度
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString out = QDir(tmp.path()).filePath(QStringLiteral("header_guard.las"));

  LasCurve dept;
  dept.name = QStringLiteral("DEPT");
  dept.values = {1.0};

  QVERIFY(LasWriter::writeLasFile(out, {dept}, {}, nullptr));

  QFile f(out);
  QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
  const QString content = QString::fromUtf8(f.readAll());
  const int cPos = content.indexOf(QStringLiteral("~C"));
  const int aPos = content.indexOf(QStringLiteral("~A"));
  QVERIFY(cPos != -1);
  QVERIFY(aPos != -1);
  QVERIFY(cPos < aPos);
}

QTEST_GUILESS_MAIN(TestIoLasWriter)
#include "tst_io_laswriter.moc"
