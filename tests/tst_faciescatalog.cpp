// tst_faciescatalog — FaciesCatalog::formationPrediction 契约（#409：#346 统一
// 口径此前零测试）。纯域函数：点要素 + 相 schema + 当前地层名 → 地层范围相结果。
#include <QtTest>
#include <QJsonDocument>

#include "domain/faciescatalog.h"

namespace {
QVariantList testSchema()
{
  return QVariantList{
      QVariantMap{{"code", 1}, {"name", QStringLiteral("分流河道")},
                  {"facies", QStringLiteral("三角洲")},
                  {"subfacies", QStringLiteral("前缘")}, {"color", "#97B4CE"}},
      QVariantMap{{"code", 2}, {"name", QStringLiteral("河口坝")},
                  {"facies", QStringLiteral("三角洲")},
                  {"subfacies", QStringLiteral("前缘")}, {"color", "#F2D28B"}}};
}

QVariantMap pointWithIntervals()
{
  QVariantMap point;
  point.insert(QStringLiteral("id"), QStringLiteral("W-1"));
  // 地层表也可走 JSON 串（DB 列读出的形态）——这里用 QVariantList 直给。
  point.insert(QStringLiteral("formation_intervals"),
               QVariantList{QVariantMap{{"name", QStringLiteral("恩平组")},
                                        {"top", 1016.0}, {"bottom", 1032.0}},
                            QVariantMap{{"name", QStringLiteral("珠海组")},
                                        {"top", 1000.0}, {"bottom", 1016.0}}});
  return point;
}
} // namespace

class TestFaciesCatalog : public QObject
{
  Q_OBJECT
private slots:
  void predictedWithinFormation()
  {
    auto point = pointWithIntervals();
    point.insert(QStringLiteral("facies_code"), 2);
    point.insert(QStringLiteral("prediction_status"), QStringLiteral("已预测"));
    const auto out =
        FaciesCatalog::formationPrediction(point, testSchema(), QStringLiteral("恩平组"));
    QCOMPARE(out.value(QStringLiteral("formation")).toString(), QStringLiteral("恩平组"));
    QCOMPARE(out.value(QStringLiteral("facies_label")).toString(), QStringLiteral("河口坝"));
    QCOMPARE(out.value(QStringLiteral("facies_code")).toInt(), 2);
    QCOMPARE(out.value(QStringLiteral("formation_top")).toDouble(), 1016.0);
    QCOMPARE(out.value(QStringLiteral("formation_base")).toDouble(), 1032.0);
    QCOMPARE(out.value(QStringLiteral("facies_scope")).toString(),
             QStringLiteral("formation"));
    // 结果行带逐层区间 JSON。
    const auto intervals = QJsonDocument::fromJson(
        out.value(QStringLiteral("predicted_intervals")).toString().toUtf8())
                               .toVariant()
                               .toList();
    QCOMPARE(intervals.size(), 1);
    QCOMPARE(intervals.first().toMap().value(QStringLiteral("code")).toInt(), 2);
  }

  void missingPredictionIsHonest()
  {
    auto point = pointWithIntervals();
    // 无 facies_code：范围内未预测——如实标「未预测」，不伪造相。
    const auto out =
        FaciesCatalog::formationPrediction(point, testSchema(), QStringLiteral("恩平组"));
    QCOMPARE(out.value(QStringLiteral("facies_label")).toString(), QStringLiteral("未预测"));
    QVERIFY(out.value(QStringLiteral("facies_code")).isNull());
    QCOMPARE(out.value(QStringLiteral("prediction_status")).toString(),
             QStringLiteral("未预测"));
    QCOMPARE(out.value(QStringLiteral("prediction_reason")).toString(),
             QStringLiteral("当前地层没有预测相"));
  }

  void unknownFormationHasNoRange()
  {
    auto point = pointWithIntervals();
    point.insert(QStringLiteral("facies_code"), 1);
    // 未知地层且点不带 formation_top/base：无有效范围 → 未预测 + 原因。
    const auto out =
        FaciesCatalog::formationPrediction(point, testSchema(), QStringLiteral("文昌组"));
    QCOMPARE(out.value(QStringLiteral("facies_label")).toString(), QStringLiteral("未预测"));
    QVERIFY(out.value(QStringLiteral("formation_top")).isNull());
    QCOMPARE(out.value(QStringLiteral("prediction_reason")).toString(),
             QStringLiteral("缺少当前地层的有效顶底深"));
  }

  void horizonEmptyFallsBackToPointFormation()
  {
    auto point = pointWithIntervals();
    point.insert(QStringLiteral("formation"), QStringLiteral("珠海组"));
    point.insert(QStringLiteral("facies_code"), 1);
    const auto out = FaciesCatalog::formationPrediction(point, testSchema(), {});
    QCOMPARE(out.value(QStringLiteral("formation")).toString(), QStringLiteral("珠海组"));
    QCOMPARE(out.value(QStringLiteral("facies_label")).toString(), QStringLiteral("分流河道"));
    QCOMPARE(out.value(QStringLiteral("formation_top")).toDouble(), 1000.0);
  }
};

QTEST_MAIN(TestFaciesCatalog)
#include "tst_faciescatalog.moc"
