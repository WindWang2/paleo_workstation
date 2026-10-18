// tst_faciescatalog — FaciesCatalog::formationPrediction 契约（#409：#346 统一
// 口径此前零测试）。纯域函数：点要素 + 相 schema + 当前地层名 → 地层范围相结果。
#include <QtTest>
#include <QJsonDocument>

#include "domain/faciescatalog.h"
#include "domain/faciestaxonomy.h"
#include "domain/facieshierarchy.h"

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
  void legacyShiftedAppearanceColumnsAreRecoveredWithoutChangingInput() {
    const QVariantMap legacy{{"code", 17}, {"name", QStringLiteral("河道充填")},
        {"facies", QStringLiteral("河流相")}, {"subfacies", QStringLiteral("河床亚相")},
        {"microfacies", "#E8D5B0"}, {"color", "strata/micro_channel_fill.svg"}, {"texture", ""}};
    QVariantList upgraded;
    QString error;
    QVERIFY2(FaciesTaxonomy::upgrade({legacy}, &upgraded, &error), qPrintable(error));
    QCOMPARE(upgraded.size(), 1);
    const auto row = upgraded.first().toMap();
    QCOMPARE(row.value("code").toInt(), 17);
    QCOMPARE(row.value("name"), legacy.value("name"));
    QCOMPARE(row.value("microfacies"), legacy.value("name"));
    QCOMPARE(row.value("facies"), legacy.value("facies"));
    QCOMPARE(row.value("subfacies"), legacy.value("subfacies"));
    QCOMPARE(row.value("color"), legacy.value("microfacies"));
    QCOMPARE(row.value("texture"), legacy.value("color"));
    QCOMPARE(legacy.value("microfacies").toString(), QString("#E8D5B0"));
    QVariantList normalized;
    QVERIFY(FaciesTaxonomy::normalize({legacy}, &normalized, &error));
    QCOMPARE(normalized, upgraded);
    const auto tree = FaciesHierarchy::legendTree({legacy});
    QCOMPARE(tree.size(), 3);
    QCOMPARE(tree.last().toMap().value("name"), legacy.value("name"));
    QCOMPARE(tree.last().toMap().value("color"), legacy.value("microfacies"));
    QCOMPARE(tree.last().toMap().value("texture"), legacy.value("color"));
    auto complete = row;
    complete.insert("microfacies", QStringLiteral("自定义微相"));
    complete.insert("name", QStringLiteral("自定义显示名"));
    QCOMPARE(FaciesTaxonomy::place(complete), complete);
  }
  void taxonomyIsAThreeLevelTree() {
    const auto schema = FaciesCatalog::defaults();
    QCOMPARE(schema.size(), 7);
    QVariantList normalized;
    QString error;
    QVERIFY2(FaciesTaxonomy::normalize(schema, &normalized, &error), qPrintable(error));
    QCOMPARE(normalized, schema);
    QVariantList codes;
    for (const auto &v : schema) {
      const auto leaf = v.toMap();
      QVERIFY(FaciesTaxonomy::complete(leaf));
      QCOMPARE(leaf.value("name"), leaf.value("microfacies"));
      QVERIFY(!FaciesCatalog::fillPath(leaf.value("texture").toString()).isEmpty());
      codes << leaf.value("code");
    }
    QCOMPARE(FaciesHierarchy::legend(schema, "facies", codes).size(), 1);
    QCOMPARE(FaciesHierarchy::legend(schema, "sub_facies", codes).size(), 2);
    const auto microLegend = FaciesHierarchy::legend(schema, "micro_facies", codes);
    QCOMPARE(microLegend.size(), 7);
    QCOMPARE(FaciesHierarchy::legend(schema, QString(), codes), microLegend);
    const auto tree = FaciesHierarchy::legendTree(microLegend);
    QCOMPARE(tree.size(), 10); // 1 相 + 2 亚相 + 7 微相
    int headings = 0;
    for (const auto &row : tree) {
      const auto f = row.toMap();
      headings += f.value("heading").toBool();
      QVERIFY(!f.value("key").toString().isEmpty());
      if (f.value("depth").toInt() > 0) QVERIFY(!f.value("parent_key").toString().isEmpty());
      QCOMPARE(f.value("depth").toInt(), f.value("heading").toBool() ? (f.value("name") == "三角洲相" ? 0 : 1) : 2);
    }
    QCOMPARE(headings, 3);
    const auto leaf = FaciesTaxonomy::resolve(schema, QStringLiteral("水下河道"));
    QCOMPARE(leaf.value("microfacies").toString(), QStringLiteral("水下分流河道"));
    QCOMPARE(leaf.value("subfacies").toString(), QStringLiteral("三角洲前缘"));
    QVERIFY(FaciesTaxonomy::resolve(schema, QStringLiteral("三角洲相")).isEmpty());
    QVERIFY(FaciesTaxonomy::resolve(schema, QStringLiteral("三角洲前缘")).isEmpty());
  }
  void taxonomyRejectsIncompleteOrDuplicatePaths() {
    QVariantList out;
    QString error;
    QVERIFY(!FaciesTaxonomy::normalize({QVariantMap{{"code", 1}, {"name", "河流相"}}}, &out, &error));
    QVERIFY(error.contains(QStringLiteral("归属")));
    QVERIFY(FaciesTaxonomy::normalize({QVariantMap{{"code", 1}, {"name", "河口坝"}}}, &out, &error));
    QCOMPARE(out[0].toMap().value("subfacies").toString(), QStringLiteral("三角洲前缘"));
    auto duplicate = out[0].toMap(); duplicate.insert("code", 2);
    QVERIFY(!FaciesTaxonomy::normalize({out[0], duplicate}, &out, &error));
    QVERIFY(!FaciesTaxonomy::normalize({QVariantMap{{"code", 3}, {"name", "河口坝"}, {"facies", "河流相"}}}, &out, &error));
  }
  void taxonomyResolvesScopedMicrofacies() {
    const QVariantList schema{
      QVariantMap{{"code", 10}, {"name", "主槽"}, {"facies", "河流"}, {"subfacies", "河道"}, {"microfacies", "主槽"}},
      QVariantMap{{"code", 11}, {"name", "主槽"}, {"facies", "三角洲"}, {"subfacies", "前缘"}, {"microfacies", "主槽"}}};
    QString error;
    QVERIFY(FaciesTaxonomy::resolve(schema, QStringLiteral("主槽"), &error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("多个归属")));
    QCOMPARE(FaciesTaxonomy::resolve(schema, QStringLiteral("河流 / 河道 / 主槽")).value("code").toInt(), 10);
    QVariantList out;
    QVERIFY(!FaciesTaxonomy::extend(schema, {QStringLiteral("主槽")}, &out, &error));
  }
  void taxonomyExpansionIsDeterministicAndNeverInventsParents() {
    const QVariantList start{FaciesCatalog::defaults()[0]};
    QVariantList a, b;
    QString error;
    QVERIFY(FaciesTaxonomy::extend(start, {QStringLiteral("天然堤"), QStringLiteral("河口坝")}, &a, &error));
    QVERIFY(FaciesTaxonomy::extend(start, {QStringLiteral("河口坝"), QStringLiteral("天然堤")}, &b, &error));
    QCOMPARE(a, b);
    QCOMPARE(a[0], start[0]);
    QCOMPARE(FaciesTaxonomy::resolve(a, QStringLiteral("天然堤")).value("subfacies").toString(), QStringLiteral("三角洲平原"));
    QVERIFY(!FaciesTaxonomy::extend(start, {QStringLiteral("三角洲前缘")}, &b, &error));
    QVERIFY(!FaciesTaxonomy::extend(start, {QStringLiteral("未知微相")}, &b, &error));
  }
  void taxonomyUpgradesLegacySchemasIntoHierarchy() {
    // 旧默认形态：平级三分类（只有相一级名称）。
    const QVariantList legacy{
        QVariantMap{{"code", 1}, {"name", "河流相"}, {"facies", "河流相"}, {"color", "#E8D5B0"}},
        QVariantMap{{"code", 2}, {"name", "三角洲相"}, {"facies", "三角洲相"}, {"color", "#E8D5B0"}},
        QVariantMap{{"code", 3}, {"name", "湖泊相"}, {"facies", "湖泊相"}, {"color", "#D5E8D0"}}};
    QVariantList upgraded;
    QString error;
    QVERIFY2(FaciesTaxonomy::upgrade(legacy, &upgraded, &error), qPrintable(error));
    QCOMPARE(upgraded.size(), 3);
    const auto pathOf = [](const QVariant &v) {
      const auto f = v.toMap();
      return QStringList{f.value("facies").toString(), f.value("subfacies").toString(),
                         f.value("microfacies").toString()};
    };
    // 相级旧类别安放到「未细分」，编码与颜色保持不变。
    const QStringList fluvial{"河流相", "未细分", "未细分"};
    const QStringList deltaic{"三角洲相", "未细分", "未细分"};
    const QStringList lacustrine{"湖泊相", "未细分", "未细分"};
    QCOMPARE(pathOf(upgraded[0]), fluvial);
    QCOMPARE(pathOf(upgraded[1]), deltaic);
    QCOMPARE(pathOf(upgraded[2]), lacustrine);
    QCOMPARE(upgraded[1].toMap().value("code").toInt(), 2);
    QCOMPARE(upgraded[1].toMap().value("color").toString(), QStringLiteral("#E8D5B0"));
    // 每条路径三级齐备：项目里不出现未定义相。
    for (const auto &v : upgraded)
      for (const auto &s : pathOf(v))
        QVERIFY2(!s.isEmpty(), qPrintable(s));
    // 幂等：已迁移的分类再次升级保持不变。
    QVariantList again;
    QVERIFY(FaciesTaxonomy::upgrade(upgraded, &again, &error));
    QCOMPARE(again, upgraded);
  }
  void taxonomyUpgradesAliasesAndUnknownLabels() {
    QVariantList upgraded;
    QString error;
    // 别名可识别：采用模板完整路径。
    const QVariantList aliased{QVariantMap{{"code", 1}, {"name", "mouth_bar"}, {"color", "#FFE082"}}};
    QVERIFY(FaciesTaxonomy::upgrade(aliased, &upgraded, &error));
    QCOMPARE(upgraded[0].toMap().value("facies").toString(), QStringLiteral("三角洲相"));
    QCOMPARE(upgraded[0].toMap().value("subfacies").toString(), QStringLiteral("三角洲前缘"));
    QCOMPARE(upgraded[0].toMap().value("microfacies").toString(), QStringLiteral("河口坝"));
    // 无法识别：落「其他 / 未归类」，原词面保留为微相。
    const QVariantList unknown{QVariantMap{{"code", 7}, {"name", "river"}, {"color", "#B4C9A0"}}};
    QVERIFY(FaciesTaxonomy::upgrade(unknown, &upgraded, &error));
    QCOMPARE(upgraded[0].toMap().value("facies").toString(), QStringLiteral("其他"));
    QCOMPARE(upgraded[0].toMap().value("subfacies").toString(), QStringLiteral("未归类"));
    QCOMPARE(upgraded[0].toMap().value("microfacies").toString(), QStringLiteral("river"));
    // 重名冲突：第二条不吞掉第一条，编号后缀去重。
    const QVariantList duplicate{
        QVariantMap{{"code", 1}, {"name", "river"}, {"color", "#B4C9A0"}},
        QVariantMap{{"code", 2}, {"name", "river"}, {"color", "#B4C9A0"}}};
    QVERIFY(FaciesTaxonomy::upgrade(duplicate, &upgraded, &error));
    QCOMPARE(upgraded[0].toMap().value("microfacies").toString(), QStringLiteral("river"));
    QVERIFY(upgraded[1].toMap().value("microfacies").toString() != QStringLiteral("river"));
    QCOMPARE(upgraded[1].toMap().value("facies").toString(), QStringLiteral("其他"));
  }
  void taxonomyPlacesSingleEntriesForDisplay() {
    // 井位图例等展示侧的逐条安放：微相可识别采用模板路径。
    const auto known = FaciesTaxonomy::place(
        QVariantMap{{"facies", "河口坝"}, {"texture", "strata/micro_mouth_bar.svg"}});
    QCOMPARE(known.value("facies").toString(), QStringLiteral("三角洲相"));
    QCOMPARE(known.value("subfacies").toString(), QStringLiteral("三角洲前缘"));
    QCOMPARE(known.value("microfacies").toString(), QStringLiteral("河口坝"));
    QCOMPARE(known.value("texture").toString(), QStringLiteral("strata/micro_mouth_bar.svg"));
    // 只有部分父级：保留父级、缺级补「未细分」。
    const auto partial = FaciesTaxonomy::place(
        QVariantMap{{"facies", "相"}, {"microfacies", "微相"}});
    QCOMPARE(partial.value("subfacies").toString(), QStringLiteral("未细分"));
    QCOMPARE(partial.value("microfacies").toString(), QStringLiteral("微相"));
    // 全空：落「其他 / 未归类 / 未分类」，三级恒有值。
    const auto empty = FaciesTaxonomy::place(QVariantMap{});
    QCOMPARE(empty.value("facies").toString(), QStringLiteral("其他"));
    QCOMPARE(empty.value("subfacies").toString(), QStringLiteral("未归类"));
    QCOMPARE(empty.value("microfacies").toString(), QStringLiteral("未分类"));
  }
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
