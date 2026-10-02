// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/curveexpr.h"

#include <cmath>
#include <limits>

using namespace paleo::curveexpr;

namespace
{
constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

CompiledExpr compileOr(const std::string &text,
                       const std::vector<std::string> &known, QString *error)
{
  std::string err;
  CompiledExpr e = CompiledExpr::compile(text, known, &err);
  if (error)
    *error = QString::fromStdString(err);
  return e;
}
} // namespace

// 容差口径：算术/比较全是双精度精确路径 → 1e-12（仅舍入）；幂/对数走
// libm → 1e-12 相对门（整数幂双精度内精确）。错误路径断言报错串关键词
// 与 offset，不断言全文（信息措辞可演进）。
class TestCurveExpr : public QObject
{
  Q_OBJECT
private slots:
  // Oracle#2 主用例：四则/优先级/括号——`DEN - 0.5*NEU` 类逐点运算。
  void testArithmeticPrecedence();
  // 幂右结合 + 一元负号优先级（-2^2=-4、2^3^2=512、2^-2=0.25）。
  void testPowerSemantics();
  // 条件掩膜 where() + 比较三值逻辑（非 NaN → 0/1）。
  void testWhereMask();
  // null 传播（≥4 用例）：算术/比较/逻辑非/min/where 条件 各一路。
  void testNullPropagation();
  // 除零 IEEE 如实：x/0 → ±∞、0/0 → NaN（不被吞，QC 面负责标记）。
  void testDivisionByZero();
  // 非法表达式报错路径（≥4 用例）：未知曲线/括号不配/尾随垃圾/未知函数/
  // 元数错/空表达式。
  void testErrorPaths();
  // 编译期变量解析：usedVariables 首现序去重；常量表达式空表。
  void testUsedVariables();
  // evaluate 缺输入曲线 → false + 指名；函数族数值抽查。
  void testEvaluateMissingAndFunctions();
};

void TestCurveExpr::testArithmeticPrecedence()
{
  QString err;
  auto e = compileOr("2 + 3 * 4", {}, &err);
  QVERIFY2(e.isValid(), qPrintable(err));
  double out[1] = {0};
  QVERIFY(e.evaluate(1, {}, out, nullptr));
  QCOMPARE(out[0], 14.0);

  e = compileOr("(2 + 3) * 4", {}, &err);
  QVERIFY(e.isValid());
  QVERIFY(e.evaluate(1, {}, out, nullptr));
  QCOMPARE(out[0], 20.0);

  e = compileOr("1.5e-3 * 2000", {}, &err);
  QVERIFY(e.evaluate(1, {}, out, nullptr));
  QCOMPARE(out[0], 3.0);

  // 目标语形：DEN − 0.5·NEU 逐点
  const double den[3] = {2.5, 2.6, 2.7};
  const double neu[3] = {0.2, 0.3, 0.4};
  e = compileOr("DEN - 0.5*NEU", {"DEN", "NEU"}, &err);
  QVERIFY2(e.isValid(), qPrintable(err));
  double o3[3] = {0, 0, 0};
  QVERIFY(e.evaluate(3, {{"DEN", den}, {"NEU", neu}}, o3, nullptr));
  QVERIFY(std::fabs(o3[0] - 2.40) < 1e-12);
  QVERIFY(std::fabs(o3[1] - 2.45) < 1e-12);
  QVERIFY(std::fabs(o3[2] - 2.50) < 1e-12);
}

void TestCurveExpr::testPowerSemantics()
{
  QString err;
  double out[1] = {0};
  auto e = compileOr("2^3^2", {}, &err); // 右结合 = 2^(3^2) = 512
  QVERIFY(e.isValid());
  QVERIFY(e.evaluate(1, {}, out, nullptr));
  QCOMPARE(out[0], 512.0);
  e = compileOr("-2^2", {}, &err); // 一元负在幂之上 = −(2²)
  QVERIFY(e.evaluate(1, {}, out, nullptr));
  QCOMPARE(out[0], -4.0);
  e = compileOr("2^-2", {}, &err); // 指数允许一元负
  QVERIFY(e.evaluate(1, {}, out, nullptr));
  QCOMPARE(out[0], 0.25);
}

void TestCurveExpr::testWhereMask()
{
  const double gr[5] = {50.0, 120.0, 80.0, kNan, 200.0};
  QString err;
  auto e = compileOr("where(GR > 100, GR, 0)", {"GR"}, &err);
  QVERIFY2(e.isValid(), qPrintable(err));
  double out[5] = {0, 0, 0, 0, 0};
  QVERIFY(e.evaluate(5, {{"GR", gr}}, out, nullptr));
  QCOMPARE(out[0], 0.0);   // 50 ≤ 100 → else 支
  QCOMPARE(out[1], 120.0); // 取 GR 本身
  QCOMPARE(out[2], 0.0);   // 边界 100 不大于 → else
  QVERIFY(std::isnan(out[3])); // 条件 NaN → NaN（三值，不是 false）
  QCOMPARE(out[4], 200.0);

  e = compileOr("GR >= 100 && GR <= 150", {"GR"}, &err);
  QVERIFY(e.evaluate(5, {{"GR", gr}}, out, nullptr));
  QCOMPARE(out[0], 0.0);
  QCOMPARE(out[1], 1.0);
  QCOMPARE(out[2], 0.0);
  QVERIFY(std::isnan(out[3]));
  QCOMPARE(out[4], 0.0);

  e = compileOr("!0 || 0", {}, &err);
  QVERIFY(e.evaluate(1, {}, out, nullptr));
  QCOMPARE(out[0], 1.0);
}

void TestCurveExpr::testNullPropagation()
{
  const double a[4] = {1.0, kNan, 3.0, 2.0};
  const double b[4] = {10.0, 5.0, kNan, 2.0};
  QString err;
  double out[4] = {0, 0, 0, 0};
  auto e = compileOr("A + B", {"A", "B"}, &err);
  QVERIFY(e.isValid());
  QVERIFY(e.evaluate(4, {{"A", a}, {"B", b}}, out, nullptr));
  QCOMPARE(out[0], 11.0);
  QVERIFY(std::isnan(out[1])); // A null
  QVERIFY(std::isnan(out[2])); // B null
  QCOMPARE(out[3], 4.0);

  e = compileOr("A > 1.5", {"A"}, &err); // 比较 NaN → NaN（非 0）
  QVERIFY(e.evaluate(4, {{"A", a}}, out, nullptr));
  QCOMPARE(out[0], 0.0);
  QVERIFY(std::isnan(out[1]));
  QCOMPARE(out[2], 1.0);

  e = compileOr("min(A, 2.0)", {"A"}, &err);
  QVERIFY(e.evaluate(4, {{"A", a}}, out, nullptr));
  QCOMPARE(out[0], 1.0);
  QVERIFY(std::isnan(out[1]));
  QCOMPARE(out[2], 2.0);

  e = compileOr("where(A >= 1, B, -1)", {"A", "B"}, &err);
  QVERIFY(e.evaluate(4, {{"A", a}, {"B", b}}, out, nullptr));
  QCOMPARE(out[0], 10.0);
  QVERIFY(std::isnan(out[1])); // 条件 null
  QVERIFY(std::isnan(out[2])); // 选中支值 null
  QCOMPARE(out[3], 2.0);
}

void TestCurveExpr::testDivisionByZero()
{
  const double z[3] = {4.0, -4.0, 0.0};
  QString err;
  auto e = compileOr("A / 0", {"A"}, &err);
  QVERIFY(e.isValid());
  double out[3] = {0, 0, 0};
  QVERIFY(e.evaluate(3, {{"A", z}}, out, nullptr));
  QVERIFY(std::isinf(out[0]) && out[0] > 0); // IEEE +∞ 如实
  QVERIFY(std::isinf(out[1]) && out[1] < 0);
  QVERIFY(std::isnan(out[2])); // 0/0 → NaN
}

void TestCurveExpr::testErrorPaths()
{
  QString err;
  QVERIFY(!compileOr("FOO + 1", {"DEN", "NEU"}, &err).isValid());
  QVERIFY2(err.contains("unknown curve") && err.contains("FOO"),
           qPrintable(err));
  QVERIFY2(err.contains("offset 0"), qPrintable(err));

  QVERIFY(!compileOr("(DEN + 1", {"DEN"}, &err).isValid());
  QVERIFY2(err.contains("')'"), qPrintable(err));

  QVERIFY(!compileOr("2 3", {}, &err).isValid());
  QVERIFY2(err.contains("trailing"), qPrintable(err));

  QVERIFY(!compileOr("sin(DEN)", {"DEN"}, &err).isValid());
  QVERIFY2(err.contains("unknown function"), qPrintable(err));

  QVERIFY(!compileOr("where(DEN, 1)", {"DEN"}, &err).isValid());
  QVERIFY2(err.contains("3 argument"), qPrintable(err));

  QVERIFY(!compileOr("", {"DEN"}, &err).isValid());
  QVERIFY2(!err.isEmpty(), "empty must fail loudly");

  QVERIFY(!compileOr("DEN +", {"DEN"}, &err).isValid());
  QVERIFY2(err.contains("unexpected end"), qPrintable(err));

  QVERIFY(!compileOr("DEN $ 1", {"DEN"}, &err).isValid());
  QVERIFY2(err.contains("unexpected character"), qPrintable(err));

  QVERIFY(!compileOr("min(DEN", {"DEN"}, &err).isValid());
  QVERIFY2(err.contains("')'"), qPrintable(err));

  // 未编译对象 evaluate → false
  CompiledExpr bad;
  double out[1] = {0};
  std::string e2;
  QVERIFY(!bad.evaluate(1, {}, out, &e2));
  QVERIFY2(!e2.empty(), "uncompiled evaluate must error");
}

void TestCurveExpr::testUsedVariables()
{
  QString err;
  auto e = compileOr("DEN - 0.5*NEU + DEN*2", {"DEN", "NEU", "GR"}, &err);
  QVERIFY(e.isValid());
  const auto used = e.usedVariables();
  QCOMPARE(static_cast<int>(used.size()), 2);
  QCOMPARE(QString::fromStdString(used[0]), QStringLiteral("DEN")); // 首现序
  QCOMPARE(QString::fromStdString(used[1]), QStringLiteral("NEU"));
  e = compileOr("1 + 2", {}, &err);
  QCOMPARE(static_cast<int>(e.usedVariables().size()), 0);
}

void TestCurveExpr::testEvaluateMissingAndFunctions()
{
  const double x[3] = {4.0, 9.0, 16.0};
  QString err;
  auto e = compileOr("sqrt(A) + ln(exp(1))", {"A"}, &err);
  QVERIFY2(e.isValid(), qPrintable(err));
  double out[3] = {0, 0, 0};
  QVERIFY(e.evaluate(3, {{"A", x}}, out, nullptr));
  QVERIFY(std::fabs(out[0] - 3.0) < 1e-12);
  QVERIFY(std::fabs(out[1] - 4.0) < 1e-12);
  QVERIFY(std::fabs(out[2] - 5.0) < 1e-12);

  // 缺输入曲线 → false + 指名
  std::string e2;
  double o2[3] = {0, 0, 0};
  QVERIFY(!e.evaluate(3, {}, o2, &e2));
  QVERIFY2(QString::fromStdString(e2).contains("missing input curve") &&
               QString::fromStdString(e2).contains("A"),
           qPrintable(QString::fromStdString(e2)));

  // clamp/max/abs/log10/pow 抽查
  e = compileOr("clamp(A, 3, 4) + max(A, 5) + abs(-1) + log10(100) + pow(A, 2)",
                {"A"}, &err);
  QVERIFY2(e.isValid(), qPrintable(err));
  const double a[2] = {9.0, 2.0};
  QVERIFY(e.evaluate(2, {{"A", a}}, out, nullptr));
  // A=9: clamp→4 + max→9 + 1 + 2 + 81 = 97
  QVERIFY(std::fabs(out[0] - 97.0) < 1e-12);
  // A=2: clamp→3 + 5 + 1 + 2 + 4 = 15
  QVERIFY(std::fabs(out[1] - 15.0) < 1e-12);
}

QTEST_MAIN(TestCurveExpr)
#include "tst_curveexpr.moc"
