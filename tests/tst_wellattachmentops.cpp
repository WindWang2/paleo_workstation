// tst_wellattachmentops — #763：井附件锚深输入解析矩阵（关键路径模块直接测试）。
//
// paleo::WellAttachmentOps::parseDepthInput 是井附件面板与锚深对话框共用的唯一
// 输入解析入口（方向 79），此前只被视图层测试间接覆盖——边界输入（.5、1,5、
// 空串、单位后缀、非正数、数值溢出）没有直接锁定。parseDepthInput 是 static 纯
// 函数，本矩阵不建 catalog、不起 QApplication，直锁拒收分类与 out 语义。
#include <QtTest>

#include "workflow/wellattachmentops.h"

#include <QString>

class WellAttachmentOpsTests : public QObject
{
    Q_OBJECT

  private slots:
    void blankClearsAnchor();
    void plainAndSuffixedNumbers();
    void leadingDotIsNotANumber();
    void commaDecimalIsBadUnit();
    void nonPositiveRejected();
    void garbageAndOverflowRejected();
    void rejectionLeavesOutUntouched();
    void reasonTextIsNonEmptyForEveryRejection();
};

void WellAttachmentOpsTests::blankClearsAnchor()
{
  // 空白输入 = 清锚（回到未锚定态），且不得写 out。
  double out = -1.0;
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QString(), &out),
           paleo::DepthInputStatus::Clear);
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("   \t "), &out),
           paleo::DepthInputStatus::Clear);
  QCOMPARE(out, -1.0);
  // out 可缺省（nullptr）——调用方只要分类。
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QString(), nullptr),
           paleo::DepthInputStatus::Clear);
}

void WellAttachmentOpsTests::plainAndSuffixedNumbers()
{
  struct Case
  {
    const char *text;
    double want;
  };
  const Case cases[] = {
    {"1500", 1500.0},
    {"1500.5", 1500.5},
    {"  1500  ", 1500.0},   // 首尾空白容忍
    {"1500 m", 1500.0},     // m 后缀
    {"1500M", 1500.0},      // 大小写不敏感
    {"1500米", 1500.0},     // 中文米
    {"+1500", 1500.0},      // 显式正号
    {"1e3", 1000.0},        // 科学计数法
  };
  for (const Case &c : cases)
  {
    const QString text = QString::fromUtf8(c.text);
    double out = 0.0;
    QVERIFY2(paleo::WellAttachmentOps::parseDepthInput(text, &out) ==
                 paleo::DepthInputStatus::Ok,
             qPrintable(QStringLiteral("input \"%1\" 未被接受").arg(text)));
    QCOMPARE(out, c.want);
  }
}

void WellAttachmentOpsTests::leadingDotIsNotANumber()
{
  // 手工口径钉死：只认「数字开头」，.5 不是合法深度写法（不是 BadUnit）。
  double out = -1.0;
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral(".5"), &out),
           paleo::DepthInputStatus::NotFinite);
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("-.5"), &out),
           paleo::DepthInputStatus::NotFinite);
}

void WellAttachmentOpsTests::commaDecimalIsBadUnit()
{
  // 中文小数点的「1,5」：正则吃掉前段 1，逗号+5 落进后缀 → 单位错误
  // （不是 NotFinite——面板据此出「不认识的单位后缀」文案）。
  double out = -1.0;
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("1,5"), &out),
           paleo::DepthInputStatus::BadUnit);
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("100ft"), &out),
           paleo::DepthInputStatus::BadUnit);
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("100千米"), &out),
           paleo::DepthInputStatus::BadUnit);
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("1.2.3"), &out),
           paleo::DepthInputStatus::BadUnit);
}

void WellAttachmentOpsTests::nonPositiveRejected()
{
  double out = -1.0;
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("0"), &out),
           paleo::DepthInputStatus::NonPositive);
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("-5"), &out),
           paleo::DepthInputStatus::NonPositive);
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("0 m"), &out),
           paleo::DepthInputStatus::NonPositive);
}

void WellAttachmentOpsTests::garbageAndOverflowRejected()
{
  double out = -1.0;
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("abc"), &out),
           paleo::DepthInputStatus::NotFinite);
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("米"), &out),
           paleo::DepthInputStatus::NotFinite);
  // 数值溢出：toDouble 给 inf，isfinite 闸拒收（不落成 inf 深度）。
  QCOMPARE(paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("1e999"), &out),
           paleo::DepthInputStatus::NotFinite);
}

void WellAttachmentOpsTests::rejectionLeavesOutUntouched()
{
  // 拒收路径不得写 out（调用方可能复用上一次的有效值）。
  double out = 1500.0;
  paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("100ft"), &out);
  QCOMPARE(out, 1500.0);
  paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("-1"), &out);
  QCOMPARE(out, 1500.0);
  paleo::WellAttachmentOps::parseDepthInput(QStringLiteral("abc"), &out);
  QCOMPARE(out, 1500.0);
}

void WellAttachmentOpsTests::reasonTextIsNonEmptyForEveryRejection()
{
  // reasonText 是唯一文案出口：Ok/Clear 无文案，三种拒收都必须有非空 reason
  // （具体措辞由翻译管，这里只锁「有/无」契约）。
  QVERIFY(paleo::WellAttachmentOps::reasonText(paleo::DepthInputStatus::Ok).isEmpty());
  QVERIFY(paleo::WellAttachmentOps::reasonText(paleo::DepthInputStatus::Clear).isEmpty());
  QVERIFY(!paleo::WellAttachmentOps::reasonText(paleo::DepthInputStatus::NotFinite).isEmpty());
  QVERIFY(!paleo::WellAttachmentOps::reasonText(paleo::DepthInputStatus::NonPositive).isEmpty());
  QVERIFY(!paleo::WellAttachmentOps::reasonText(paleo::DepthInputStatus::BadUnit).isEmpty());
}

QTEST_APPLESS_MAIN(WellAttachmentOpsTests)
#include "tst_wellattachmentops.moc"
