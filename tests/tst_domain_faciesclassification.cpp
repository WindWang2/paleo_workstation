#include <QtTest>

#include "../src/domain/faciesclassification.h"

using namespace paleo::crossplot;

class TestDomainFaciesClassification : public QObject
{
  Q_OBJECT

private slots:
  void classificationOptionsDefaults();
  void classColorModuloPalette();
  void classificationDataStructures();
  void wellIntervalGeometry();
  void faciesProductProperties();
  void qMetaTypeDeclaration();
  void mutationDemonstration_paletteCycle();
};

void TestDomainFaciesClassification::classificationOptionsDefaults()
{
  ClassificationOptions opt;
  QCOMPARE(opt.method, Classifier::KMeans);
  QCOMPARE(opt.k, 8);
  QCOMPARE(opt.maxIterations, 100);
  QCOMPARE(opt.manualClass, 0);
  QCOMPARE(opt.seed, 42ULL);
  QCOMPARE(opt.standardize, true);
  QVERIFY(opt.selection.isEmpty());
}

void TestDomainFaciesClassification::classColorModuloPalette()
{
  // 检查首项颜色
  const ClassColor c0 = classColor(0);
  QCOMPARE(c0.red, 166);
  QCOMPARE(c0.green, 206);
  QCOMPARE(c0.blue, 227);

  // 检查第 1 项
  const ClassColor c1 = classColor(1);
  QCOMPARE(c1.red, 31);
  QCOMPARE(c1.green, 120);
  QCOMPARE(c1.blue, 180);

  // 检查模 12 循环：12 与 0 相同，13 与 1 相同
  const ClassColor c12 = classColor(12);
  QCOMPARE(c12.red, c0.red);
  QCOMPARE(c12.green, c0.green);
  QCOMPARE(c12.blue, c0.blue);

  const ClassColor c13 = classColor(13);
  QCOMPARE(c13.red, c1.red);
  QCOMPARE(c13.green, c1.green);
  QCOMPARE(c13.blue, c1.blue);
}

void TestDomainFaciesClassification::classificationDataStructures()
{
  Classification res;
  QCOMPARE(res.ok, false);
  QCOMPARE(res.cancelled, false);
  QVERIFY(res.labels.empty());

  res.ok = true;
  res.labels = {0, 1, 1, 2};
  res.confidence = {0.9, 0.8, 0.85, 0.95};
  res.counts = {1, 2, 1};

  QCOMPARE(res.labels.size(), 4);
  QCOMPARE(res.confidence.size(), 4);
  QCOMPARE(res.counts.size(), 3);
}

void TestDomainFaciesClassification::wellIntervalGeometry()
{
  WellInterval wi;
  QCOMPARE(wi.classId, -1);
  QCOMPARE(wi.top, 0.0);
  QCOMPARE(wi.base, 0.0);
  QCOMPARE(wi.sampleCount, 0);

  wi.wellId = QStringLiteral("W-1");
  wi.top = 1000.5;
  wi.base = 1050.0;
  wi.classId = 2;
  wi.sampleCount = 100;
  wi.meanConfidence = 0.88;

  QCOMPARE(wi.wellId, QStringLiteral("W-1"));
  QCOMPARE(wi.base - wi.top, 49.5);
  QCOMPARE(wi.sampleCount, 100);
}

void TestDomainFaciesClassification::faciesProductProperties()
{
  FaciesProduct prod;
  QCOMPARE(prod.ok, false);
  prod.ok = true;
  prod.assetId = QStringLiteral("ast-facies-1");
  prod.counts = {10, 20, 30};

  QCOMPARE(prod.ok, true);
  QCOMPARE(prod.assetId, QStringLiteral("ast-facies-1"));
  QCOMPARE(prod.counts.size(), 3);
}

void TestDomainFaciesClassification::qMetaTypeDeclaration()
{
  const QMetaType mt = QMetaType::fromType<ClassificationOptions>();
  QVERIFY(mt.isValid());
}

void TestDomainFaciesClassification::mutationDemonstration_paletteCycle()
{
  // 变异测试示范：调色板必须严格在 12 个离散色间循环
  for (int code = 0; code < 36; ++code)
  {
    const ClassColor current = classColor(code);
    const ClassColor expected = classColor(code % 12);
    QCOMPARE(current.red, expected.red);
    QCOMPARE(current.green, expected.green);
    QCOMPARE(current.blue, expected.blue);
  }
  // 相邻两项颜色绝不相同
  const ClassColor c0 = classColor(0);
  const ClassColor c1 = classColor(1);
  QVERIFY(c0.red != c1.red || c0.green != c1.green || c0.blue != c1.blue);
}

QTEST_GUILESS_MAIN(TestDomainFaciesClassification)
#include "tst_domain_faciesclassification.moc"
