// #740：后台影响分析/重算对比终态文案——成败判定与关键数字不丢。
#include "ui/workbench/impactoutcome.h"
#include <QtTest>

using namespace paleo::workbench;

class TstWorkbenchImpactOutcome : public QObject {
  Q_OBJECT
private slots:
  void impactFailureKeepsError() {
    const auto out = describeImpactOutcome({}, QStringLiteral("已取消"));
    QVERIFY(!out.ok);
    QCOMPARE(out.message, QStringLiteral("已取消"));
    QVERIFY(!describeImpactOutcome({}, {}).message.isEmpty());
  }
  void impactSuccessSumsCells() {
    const QVariantMap report{
        {"determined", QVariantList{QVariantMap{{"changed_cells", 7}},
                                    QVariantMap{{"changed_cells", 5}}}},
        {"direct_products", QVariantList{1, 2}},
        {"potential_products", QVariantList{3}},
        {"report_path", QStringLiteral("r.json")}};
    const auto out = describeImpactOutcome(report, {});
    QVERIFY(out.ok);
    QVERIFY(out.message.contains("12"));
    QVERIFY(out.message.contains("r.json"));
  }
  void compareTopTransitionsSorted() {
    const QVariantMap report{
        {"cells_changed", 9}, {"total_cells", 100}, {"report_path", "c.json"},
        {"code_matrix",
         QVariantList{QVariantMap{{"transition", "a"}, {"cells", 1}},
                      QVariantMap{{"transition", "b"}, {"cells", 8}}}}};
    const auto out = describeCompareOutcome(report, {});
    QVERIFY(out.ok);
    QVERIFY(out.message.indexOf("b") < out.message.indexOf("a："));
    QVERIFY(!describeCompareOutcome({}, {}).ok);
  }
};

QTEST_GUILESS_MAIN(TstWorkbenchImpactOutcome)
#include "tst_workbench_impactoutcome.moc"
