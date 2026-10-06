#include <QtTest>
#include <cmath>
#include <vector>

#include "../src/algorithms/geostat/linsolve.h"

using namespace paleo::geostat;

class TestGeostatLinSolve : public QObject
{
  Q_OBJECT

private slots:
  void identityMatrixSolve();
  void standard2x2And3x3Systems();
  void borderedKrigingIndefiniteMatrix();
  void singularMatrixFails();
  void dimensionOne();
  void mutationDemonstration_residualPrecision();
};

void TestGeostatLinSolve::identityMatrixSolve()
{
  const int n = 4;
  std::vector<double> a(n * n, 0.0);
  for (int i = 0; i < n; ++i)
  {
    a[i * n + i] = 1.0;
  }
  const std::vector<double> b = {1.5, -2.5, 3.0, 0.0};
  std::vector<double> x(n, 0.0);

  QVERIFY(solveDenseLu(a, n, b, x));
  for (int i = 0; i < n; ++i)
  {
    QCOMPARE(x[i], b[i]);
  }
}

void TestGeostatLinSolve::standard2x2And3x3Systems()
{
  // 2x2:
  // [3, 2] * [x0] = [7]  -> x0 = 1
  // [1, 4]   [x1]   [9]  -> x1 = 2
  const std::vector<double> a2 = {3.0, 2.0, 1.0, 4.0};
  const std::vector<double> b2 = {7.0, 9.0};
  std::vector<double> x2(2, 0.0);

  QVERIFY(solveDenseLu(a2, 2, b2, x2));
  QVERIFY(std::abs(x2[0] - 1.0) < 1e-12);
  QVERIFY(std::abs(x2[1] - 2.0) < 1e-12);

  // 3x3:
  // [1, 2, 3] * [x] = [14]  -> [1, 2, 3]
  // [2, 5, 3]         [23]
  // [1, 0, 8]         [25]
  const std::vector<double> a3 = {
    1.0, 2.0, 3.0,
    2.0, 5.0, 3.0,
    1.0, 0.0, 8.0
  };
  const std::vector<double> b3 = {14.0, 23.0, 25.0};
  std::vector<double> x3(3, 0.0);

  QVERIFY(solveDenseLu(a3, 3, b3, x3));
  QVERIFY(std::abs(x3[0] - 1.0) < 1e-10);
  QVERIFY(std::abs(x3[1] - 2.0) < 1e-10);
  QVERIFY(std::abs(x3[2] - 3.0) < 1e-10);
}

void TestGeostatLinSolve::borderedKrigingIndefiniteMatrix()
{
  // 普通克里金加边不定矩阵（gamma(0)=0，对角含 0）：
  // [[0, g12, 1],
  //  [g12, 0, 1],
  //  [1,   1, 0]]
  const double g = 0.5;
  const std::vector<double> a = {
    0.0, g,   1.0,
    g,   0.0, 1.0,
    1.0, 1.0, 0.0
  };
  // 等权且 Lagrange 乘子满足对称性
  const std::vector<double> b = {g / 2.0, g / 2.0, 1.0};
  std::vector<double> x(3, 0.0);

  QVERIFY(solveDenseLu(a, 3, b, x));
  // 权重和应为 1 (x[0] + x[1] == 1.0)
  QVERIFY(std::abs(x[0] + x[1] - 1.0) < 1e-12);
  QVERIFY(std::abs(x[0] - 0.5) < 1e-12);
  QVERIFY(std::abs(x[1] - 0.5) < 1e-12);
}

void TestGeostatLinSolve::singularMatrixFails()
{
  // 奇异矩阵：线性相关行
  const std::vector<double> a = {
    1.0, 2.0,
    2.0, 4.0
  };
  const std::vector<double> b = {3.0, 6.0};
  std::vector<double> x(2, 999.0);

  const bool ok = solveDenseLu(a, 2, b, x);
  QVERIFY(!ok);
}

void TestGeostatLinSolve::dimensionOne()
{
  const std::vector<double> a = {5.0};
  const std::vector<double> b = {15.0};
  std::vector<double> x(1, 0.0);

  QVERIFY(solveDenseLu(a, 1, b, x));
  QCOMPARE(x[0], 3.0);
}

void TestGeostatLinSolve::mutationDemonstration_residualPrecision()
{
  // 变异测试示范：解向量代回方程，残差 ||A*x - b|| 无穷范数必须小于 1e-10
  const int n = 3;
  const std::vector<double> a = {
    4.0, 1.0, -1.0,
    2.0, 7.0, 1.0,
    1.0, -3.0, 12.0
  };
  const std::vector<double> b = {13.0, -8.0, 32.0};
  std::vector<double> x(n, 0.0);

  QVERIFY(solveDenseLu(a, n, b, x));

  for (int i = 0; i < n; ++i)
  {
    double rowSum = 0.0;
    for (int j = 0; j < n; ++j)
    {
      rowSum += a[i * n + j] * x[j];
    }
    const double res = std::abs(rowSum - b[i]);
    QVERIFY2(res < 1e-10, qPrintable(QStringLiteral("Row %1 residual %2").arg(i).arg(res)));
  }
}

QTEST_GUILESS_MAIN(TestGeostatLinSolve)
#include "tst_geostat_linsolve.moc"
