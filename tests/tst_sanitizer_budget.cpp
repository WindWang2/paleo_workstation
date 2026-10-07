// 降档开关单测（方向70）：同一源码编两个二进制——默认（无定义，系数=1，
// 预算原值）与强制 PALEO_SANITIZER_BUILD=1（系数=3，预算×3，CMake 侧
// target_compile_definitions）——本机（含 MSVC，sanitizer 档本机不可跑）即可
// 验证 on/off 两档预算差与接线。三个 budget 测试（tst_correlation_full/
// tst_perfbudget/tst_segy_lines）的放宽走同一 helper，此处钉住契约：
// 系数取值、放宽是严格线性乘法、非 sanitizer 档基线预算逐值不变。
#include <QCoreApplication>
#include <QtTest>

#include "perfbudget_relax.h"

using namespace paleo::perfbudget;

namespace
{
// 三个 budget 测试的基线预算全集（ms）：perfbudget 100/500/100、
// correlation_full 1000(warm reorder)/3000/9000/12000。
constexpr qint64 kAllBudgets[] = {100, 500, 1000, 3000, 9000, 12000};
}

class TestSanitizerBudget : public QObject
{
  Q_OBJECT

private slots:
  void factorMatchesBuildMode()
  {
#ifdef PALEO_SANITIZER_BUILD
    QCOMPARE(sanitizerRelaxFactor(), 3);
#else
    QCOMPARE(sanitizerRelaxFactor(), 1);
#endif
  }

  void relaxIsLinearForEveryBudget()
  {
    const int f = sanitizerRelaxFactor();
    for (const qint64 base : kAllBudgets)
    {
      QCOMPARE(relaxedBudgetMs(base), base * f);
      QVERIFY(relaxedBudgetMs(base) >= base);
    }
  }

#ifndef PALEO_SANITIZER_BUILD
  void strictBuildKeepsBudgetsUntouched()
  {
    // 非 sanitizer 对拍锚点：每一档基线预算必须原值通过（本二进制与
    // tst_sanitizer_budget_on 各钉一档，两绿合起来 = on/off 预算差证据）。
    for (const qint64 base : kAllBudgets)
      QCOMPARE(relaxedBudgetMs(base), base);
  }
#else
  void sanitizerBuildRelaxesEveryBudget()
  {
    for (const qint64 base : kAllBudgets)
      QCOMPARE(relaxedBudgetMs(base), base * 3);
  }
#endif
};

int main(int argc, char *argv[])
{
  QCoreApplication app(argc, argv);
  TestSanitizerBudget tc;
  return QTest::qExec(&tc, argc, argv) == 0 ? 0 : 1;
}

#include "tst_sanitizer_budget.moc"
