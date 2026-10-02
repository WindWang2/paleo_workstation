// 层：数据（测试壳位于 tests/，被测对象为数据层纯数值核）
#include <QtTest/QtTest>

#include "algorithms/rasteralgebra.h"

#include <cmath>
#include <limits>
#include <vector>

using namespace paleo::rasteralgebra;

namespace
{

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

std::vector<float> makeRaster(std::initializer_list<float> vals)
{
  return std::vector<float>(vals);
}

} // namespace

class TestRasterAlgebra : public QObject
{
  Q_OBJECT

private slots:
  // Oracle 2（≥5 用例，含 null 传播与零除）。
  void addSubtractMultiply();
  void divideByZeroYieldsNull();
  void nullPropagationAllOps();
  void minMax();
  void constantOps();
  void unaryOps();
  void compareAndWhere();
  void inPlaceAliases(); // out 与输入同缓冲亦安全
};

void TestRasterAlgebra::addSubtractMultiply()
{
  const auto a = makeRaster({1.0f, 2.0f, kNan, 4.0f});
  const auto b = makeRaster({10.0f, 20.0f, 30.0f, kNan});
  std::vector<float> out(4);
  applyBinary(a.data(), b.data(), 4, BinaryOp::Add, out.data());
  QCOMPARE(out[0], 11.0f);
  QCOMPARE(out[1], 22.0f);
  QVERIFY(std::isnan(out[2]));
  QVERIFY(std::isnan(out[3]));

  applyBinary(a.data(), b.data(), 4, BinaryOp::Subtract, out.data());
  QCOMPARE(out[0], -9.0f);
  QCOMPARE(out[1], -18.0f);

  applyBinary(a.data(), b.data(), 4, BinaryOp::Multiply, out.data());
  QCOMPARE(out[0], 10.0f);
  QCOMPARE(out[1], 40.0f);
}

void TestRasterAlgebra::divideByZeroYieldsNull()
{
  const auto a = makeRaster({1.0f, -1.0f, 3.0f, kNan});
  const auto b = makeRaster({0.0f, 0.0f, 2.0f, 0.0f});
  std::vector<float> out(4);
  applyBinary(a.data(), b.data(), 4, BinaryOp::Divide, out.data());
  QVERIFY2(std::isnan(out[0]), "1/0 must be null, not +inf");
  QVERIFY2(std::isnan(out[1]), "-1/0 must be null, not -inf");
  QCOMPARE(out[2], 1.5f);
  QVERIFY(std::isnan(out[3]));
  // 常数零除同语义（除以常数 0 → 全 null）。
  applyConstant(a.data(), 4, 0.0, BinaryOp::Divide, out.data());
  QVERIFY(std::isnan(out[0]));
  QVERIFY(std::isnan(out[2]));
}

void TestRasterAlgebra::nullPropagationAllOps()
{
  const std::vector<float> withNull = {kNan, 1.0f};
  const std::vector<float> plain = {5.0f, 5.0f};
  std::vector<float> out(2);
  for (int op = 0; op < 6; ++op)
  {
    applyBinary(withNull.data(), plain.data(), 2, static_cast<BinaryOp>(op), out.data());
    QVERIFY2(std::isnan(out[0]),
             qPrintable(QStringLiteral("op %1 must propagate null").arg(op)));
    QVERIFY(!std::isnan(out[1]));
    applyBinary(plain.data(), withNull.data(), 2, static_cast<BinaryOp>(op), out.data());
    QVERIFY2(std::isnan(out[0]),
             qPrintable(QStringLiteral("op %1 must propagate null (rhs)").arg(op)));
  }
}

void TestRasterAlgebra::minMax()
{
  const auto a = makeRaster({1.0f, 5.0f, kNan, -3.0f});
  const auto b = makeRaster({2.0f, -8.0f, 1.0f, kNan});
  std::vector<float> out(4);
  applyBinary(a.data(), b.data(), 4, BinaryOp::Min, out.data());
  QCOMPARE(out[0], 1.0f);
  QCOMPARE(out[1], -8.0f);
  QVERIFY(std::isnan(out[2]));
  applyBinary(a.data(), b.data(), 4, BinaryOp::Max, out.data());
  QCOMPARE(out[0], 2.0f);
  QCOMPARE(out[1], 5.0f);
}

void TestRasterAlgebra::constantOps()
{
  const auto a = makeRaster({1.0f, kNan, 3.0f});
  std::vector<float> out(3);
  applyConstant(a.data(), 3, 2.0, BinaryOp::Add, out.data());
  QCOMPARE(out[0], 3.0f);
  QVERIFY(std::isnan(out[1]));
  QCOMPARE(out[2], 5.0f);
  applyConstant(a.data(), 3, 2.0, BinaryOp::Multiply, out.data());
  QCOMPARE(out[0], 2.0f);
  applyConstant(a.data(), 3, 0.5, BinaryOp::Divide, out.data());
  QCOMPARE(out[2], 6.0f);
}

void TestRasterAlgebra::unaryOps()
{
  const auto a = makeRaster({-2.0f, kNan, 3.0f});
  std::vector<float> out(3);
  applyUnary(a.data(), 3, UnaryOp::Negate, out.data());
  QCOMPARE(out[0], 2.0f);
  QVERIFY(std::isnan(out[1]));
  QCOMPARE(out[2], -3.0f);
  applyUnary(a.data(), 3, UnaryOp::Abs, out.data());
  QCOMPARE(out[0], 2.0f);
  QCOMPARE(out[2], 3.0f);
}

void TestRasterAlgebra::compareAndWhere()
{
  const auto a = makeRaster({1.0f, 5.0f, kNan, 7.0f});
  const auto b = makeRaster({2.0f, 5.0f, 1.0f, kNan});
  std::vector<std::uint8_t> mask(4);
  compareConstant(a.data(), 4, 4.0, CompareOp::Greater, mask.data());
  QCOMPARE(mask[0], 0);
  QCOMPARE(mask[1], 1);
  QCOMPARE(mask[2], 0); // NaN 比较恒假
  QCOMPARE(mask[3], 1);

  compareRasters(a.data(), b.data(), 4, CompareOp::GreaterOrEqual, mask.data());
  QCOMPARE(mask[0], 0);
  QCOMPARE(mask[1], 1);
  QCOMPARE(mask[2], 0);
  QCOMPARE(mask[3], 0); // NaN 任一侧 → 假

  const auto hi = makeRaster({100.0f, 100.0f, 100.0f, 100.0f});
  const auto lo = makeRaster({-1.0f, -1.0f, -1.0f, -1.0f});
  std::vector<float> out(4);
  where(mask.data(), hi.data(), lo.data(), 4, out.data());
  QCOMPARE(out[0], -1.0f);
  QCOMPARE(out[1], 100.0f);
  QCOMPARE(out[3], -1.0f);
  // 被选中源 NaN → NaN；未选中源 NaN 不影响输出。
  const auto hiNan = makeRaster({kNan, 100.0f, 100.0f, 100.0f});
  const auto loNan = makeRaster({-1.0f, -1.0f, kNan, -1.0f});
  where(mask.data(), hiNan.data(), loNan.data(), 4, out.data());
  QCOMPARE(out[0], -1.0f); // mask=0 → 选 lo（有限）
  QCOMPARE(out[1], 100.0f);
  QVERIFY(std::isnan(out[2])); // mask=0 → 选 lo，lo NaN 传播
}

void TestRasterAlgebra::inPlaceAliases()
{
  auto a = makeRaster({1.0f, 2.0f, 3.0f});
  std::vector<float> b = {4.0f, 5.0f, 6.0f};
  applyBinary(a.data(), b.data(), 3, BinaryOp::Add, a.data()); // out 别名 a
  QCOMPARE(a[0], 5.0f);
  QCOMPARE(a[2], 9.0f);
  applyConstant(a.data(), 3, 2.0, BinaryOp::Multiply, a.data());
  QCOMPARE(a[0], 10.0f);
}

QTEST_MAIN(TestRasterAlgebra)
#include "tst_rasteralgebra.moc"
