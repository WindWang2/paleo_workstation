// 层：测试壳
// tst_dataquery — P2 统一查询纯值面 oracle 回归：DataQuery 编辑语义（W1）、
// CompiledQuery 匹配/编译计数（W2）、列约束集合语义与候选（W3）、状态串
// v2/legacy/诊断（W4）。期望集全部手写（显式 ID 集），不调用被测 matcher
// 反推期望——同值同名不同 ID、类型覆盖、空关联、多实体、标签、未决、软删
// 都有独立行。
#include <QtTest>

#include "../src/ui/pages/dataops/dataopsquery.h"

using namespace paleo::dataops;
using namespace paleo::dataops::query;

namespace {

// 确定性夹具：7 行覆盖验收矩阵要求的形态。期望集按行属性手推。
QVector<AssetRowInfo> fixtureRows()
{
  QVector<AssetRowInfo> rows;
  auto mk = [&rows](const QString &id, const QString &name, const QString &effType,
                    const QString &status, const QStringList &entities,
                    const QStringList &tags, bool unresolved,
                    const QString &fileName = QString()) {
    AssetRowInfo r;
    r.assetId = id;
    r.displayName = name;
    r.fileName = fileName.isEmpty() ? name : fileName;
    r.type = effType;
    r.effectiveType = effType;
    r.status = status;
    r.entityNames = entities;
    r.tags = tags;
    r.unresolved = unresolved;
    r.currentVersionNo = 1;
    rows.append(r);
    return r;
  };
  mk("a1", "Alpha.las", "well_log", "RAW", {"W1"}, {"核心"}, false);
  mk("a2", "Beta.las", "tops", "DERIVED", {"W1", "W2"}, {}, true);
  mk("a3", "Beta.las", "well_log", "RAW", {}, {"核心", "区域"}, false); // 与 a2 同名不同 ID
  mk("a4", "Gamma.sgy", "seismic", "RAW", {"W2"}, {}, false,
     "!special#1&2.las"); // 文件名带保留字符
  // a5：catalog 原型 seismic、用户覆盖为 horizon（effectiveType 口径）。
  AssetRowInfo a5;
  a5.assetId = "a5";
  a5.displayName = "Delta.grd";
  a5.fileName = "Delta.grd";
  a5.type = "seismic";
  a5.effectiveType = "horizon";
  a5.status = "DERIVED";
  a5.currentVersionNo = 2;
  rows.append(a5);
  mk("a6", "Eps[1].las", "tops", "RAW", {"W3"}, {}, false); // 字面量 '[' 命中面
  mk("a7", "Zeta.las", "", "RAW", {}, {}, false);           // 空类型（类型未知口径）
  return rows;
}

QSet<QString> idsMatching(const DataQuery &q, const QVector<AssetRowInfo> &rows)
{
  const CompiledQuery c = CompiledQuery::compile(q);
  QSet<QString> out;
  for (const AssetRowInfo &r : rows)
    if (c.matches(r))
      out.insert(r.assetId);
  return out;
}

QSet<QString> set(std::initializer_list<const char *> ids)
{
  QSet<QString> out;
  for (const char *id : ids)
    out.insert(QLatin1String(id));
  return out;
}

} // namespace

class TestDataQuery : public QObject
{
  Q_OBJECT

private slots:
  // ---- W1：否定条件在旧控件镜像语义下存活 ------------------------------
  void negativeOnlyConditionsSurviveLegacyEdit()
  {
    DataQuery q;
    q.group.add({FilterDim::Search, QStringLiteral("alpha"), true});  // 非搜索 alpha
    q.group.add({FilterDim::Type, QStringLiteral("seismic"), true});  // 非类型 seismic
    // 旧输入框为空 → setPositiveValue(dim, "") 必须是 no-op（原 clearDim 会删掉它们）。
    QVERIFY(!q.setPositiveValue(FilterDim::Search, QString()));
    QVERIFY(!q.setPositiveValue(FilterDim::Type, QString()));
    QCOMPARE(q.group.conditions.size(), 2);
    // 手推 oracle：非 alpha 搜索 + 非 seismic 类型（effectiveType 口径）。
    // a1 Alpha 命中搜索→排除；a4 seismic→排除；a5 horizon 且名不含 alpha→留。
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a2", "a3", "a5", "a6", "a7"}));
    // 正向编辑只动首个正向条件，否定保留。
    QVERIFY(q.setPositiveValue(FilterDim::Type, QStringLiteral("tops")));
    QCOMPARE(q.group.conditions.size(), 3);
    QVERIFY(q.group.conditions.at(0).dim == FilterDim::Type);
    QVERIFY(!q.group.conditions.at(0).negate);
    QVERIFY(q.group.conditions.at(2).negate); // 非 seismic 仍在
    // tops 且 非 seismic：a2 tops✓（DERIVED 无关）、a6 tops✓；a5 horizon✗。
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a2", "a6"}));
    // 清空正向 → 只剩否定条件。
    QVERIFY(q.setPositiveValue(FilterDim::Type, QString()));
    QCOMPARE(q.group.conditions.size(), 2);
  }

  void legacyEditKeepsSameDimMultiValues()
  {
    DataQuery q; // 用户经 FilterBar 有意加了两个正向 Tag 条件。
    q.group.add({FilterDim::Tag, QStringLiteral("核心"), false});
    q.group.add({FilterDim::Tag, QStringLiteral("区域"), false});
    // AND 下两个标签同时持有 → 手推：a3（核心+区域）。
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a3"}));
    // 旧搜索框编辑另一维：多值不动。
    QVERIFY(q.setPositiveValue(FilterDim::Search, QStringLiteral("Beta")));
    QCOMPARE(q.group.conditions.size(), 3);
    QVERIFY(q.group.conditions.at(0).dim == FilterDim::Search); // 新正向前置
    // Beta 名字 + 两标签：a2 无标签✗、a3 全中✓。
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a3"}));
  }

  void chipEditTargetsSingleCondition()
  {
    DataQuery q;
    QVERIFY(q.addCondition({FilterDim::Type, QStringLiteral("well_log"), false}));
    QVERIFY(!q.addCondition({FilterDim::Type, QStringLiteral("well_log"), false})); // 去重
    QVERIFY(q.addCondition({FilterDim::Type, QStringLiteral("tops"), false}));
    QCOMPARE(q.group.conditions.size(), 2);
    // 取反首个匹配：只翻转 well_log 一条（原实现会把同值条件全部翻转——
    // 去重后仍需保证按身份单条操作）。
    QVERIFY(q.toggleFirstMatching({FilterDim::Type, QStringLiteral("well_log"), false}));
    QCOMPARE(q.group.conditions.at(0).negate, true);
    QCOMPARE(q.group.conditions.at(1).negate, false);
    // 删首个匹配：只删一条。
    QVERIFY(q.removeFirstMatching({FilterDim::Type, QStringLiteral("well_log"), true}));
    QCOMPARE(q.group.conditions.size(), 1);
    QCOMPARE(q.group.conditions.at(0).value, QStringLiteral("tops"));
  }

  void setSwitchPreservesNegatedSibling()
  {
    DataQuery q;
    q.group.add({FilterDim::Warned, QString(), true}); // 非有警告
    QVERIFY(!q.setSwitch(FilterDim::Warned, false));   // off 时正向本就不在 → no-op
    QCOMPARE(q.group.conditions.size(), 1);
    QVERIFY(q.setSwitch(FilterDim::Warned, true));
    QCOMPARE(q.group.conditions.size(), 2);
    QVERIFY(q.hasPositive(FilterDim::Warned));
    QVERIFY(q.setSwitch(FilterDim::Warned, false));    // 只删正向
    QCOMPARE(q.group.conditions.size(), 1);
    QVERIFY(q.group.conditions.at(0).negate);
    QVERIFY(!q.hasPositive(FilterDim::Warned));
  }

  // ---- W2：混合条件 oracle + 编译计数 ----------------------------------
  void mixedPositiveNegativeOracle()
  {
    DataQuery q; // 类型 well_log 且 非状态 DERIVED。
    q.group.add({FilterDim::Type, QStringLiteral("well_log"), false});
    q.group.add({FilterDim::Status, QStringLiteral("DERIVED"), true});
    // 手推：a1 well_log/RAW✓；a3 well_log/RAW✓；其余类型不符。
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a1", "a3"}));
  }

  void orGroupSemantics()
  {
    DataQuery q; // 类型 well_log OR 类型 tops。
    q.group.add({FilterDim::Type, QStringLiteral("well_log"), false});
    q.group.add({FilterDim::Type, QStringLiteral("tops"), false});
    q.group.orMode = true;
    // 手推：a1/a3 well_log，a2/a6 tops。
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a1", "a2", "a3", "a6"}));
  }

  void searchHitsTypesEntitiesAndFile()
  {
    DataQuery q; // 搜索词命中 effectiveType。
    q.group.add({FilterDim::Search, QStringLiteral("horizon"), false});
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a5"}));
    q.group.conditions.clear();
    q.group.add({FilterDim::Search, QStringLiteral("w2"), false}); // 命中关联实体
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a2", "a4"}));
    q.group.conditions.clear();
    q.group.add({FilterDim::Search, QStringLiteral("!special"), false}); // 命中文件名（含 !）
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a4"}));
  }

  void regexCompileCountScalesWithConditionsNotRows()
  {
    DataQuery q; // 2 个有效正则 + 1 个无效正则（字面量回退）。
    q.group.add({FilterDim::Regex, QStringLiteral("^Alpha"), false});
    q.group.add({FilterDim::Regex, QStringLiteral(".*\\.sgy$"), false});
    q.group.add({FilterDim::Regex, QStringLiteral("Eps[1"), false}); // 无效模式
    QVector<AssetRowInfo> rows;
    for (int i = 0; i < 500; ++i)
      rows.append(fixtureRows().at(i % 7)); // 500 行扫描同一编译产物
    const CompiledQuery c = CompiledQuery::compile(q);
    QCOMPARE(c.regexCompileCount(), 3);
    int hits = 0;
    for (const AssetRowInfo &r : rows)
      if (c.matches(r))
        ++hits;
    QCOMPARE(c.regexCompileCount(), 3); // 扫描不增加编译次数
    // 手推：AND 三个条件同时成立——只有 a4 满足？逐条：^Alpha 命中 a1 名；
    // .*\.sgy$ 命中 a4 名/Gamma.sgy；AND 无交集 → 0。改为验证 AND 语义：
    QCOMPARE(hits, 0);
    // 无效正则按字面量「Eps[1」匹配 a6 的文件名；单独验证。
    DataQuery q2;
    q2.group.add({FilterDim::Regex, QStringLiteral("Eps[1"), false});
    const CompiledQuery c2 = CompiledQuery::compile(q2);
    bool literalFallbackListed = false;
    for (const auto &s : c2.summaries())
      if (s.regexLiteralFallback && s.text.contains(QStringLiteral("字面量")))
        literalFallbackListed = true;
    QVERIFY(literalFallbackListed); // 解释的与执行的同一准备产物
    QCOMPARE(idsMatching(q2, fixtureRows()), set({"a6"}));
  }

  // ---- W3：列约束集合语义 -----------------------------------------------
  void columnSameOrCrossAndWithGroup()
  {
    DataQuery q; // 原组 OR（well_log/tops）+ 列类型 {tops,seismic}。
    q.group.add({FilterDim::Type, QStringLiteral("well_log"), false});
    q.group.add({FilterDim::Type, QStringLiteral("tops"), false});
    q.group.orMode = true;
    q.setColumnValues(ColumnKey::Type,
                      {QStringLiteral("seismic"), QStringLiteral("tops")});
    // 组结果 {a1,a2,a3,a6} ∩ 类型∈{tops,seismic}（effectiveType）= {a2,a6}。
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a2", "a6"}));
    // 跨列 AND：再限关联 {W1} → {a2}（a2 实体 W1,W2 含 W1）。
    q.setColumnValues(ColumnKey::Entity, {QStringLiteral("W1")});
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a2"}));
    // 关联按 entityNames 逐值：拼接串「W1、W2」不是任何行的一个实体名 → 空。
    q.setColumnValues(ColumnKey::Entity, {QStringLiteral("W1、W2")});
    QCOMPARE(idsMatching(q, fixtureRows()), set({}));
  }

  void columnEmptySetVersusUnrestricted()
  {
    DataQuery q;
    q.setColumnValues(ColumnKey::Type, {QStringLiteral("well_log")});
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a1", "a3"}));
    q.clearColumn(ColumnKey::Type); // 未设置 = 不限
    QVERIFY(q.column(ColumnKey::Type) == nullptr);
    QCOMPARE(idsMatching(q, fixtureRows()),
             set({"a1", "a2", "a3", "a4", "a5", "a6", "a7"}));
    q.setColumnValues(ColumnKey::Type, {}); // 显式空集合 = 无匹配
    QVERIFY(q.column(ColumnKey::Type) != nullptr);
    QVERIFY(q.column(ColumnKey::Type)->values.isEmpty());
    QCOMPARE(idsMatching(q, fixtureRows()), set({}));
    QVERIFY(q.column(ColumnKey::Type)->display().contains(QStringLiteral("无匹配")));
  }

  void columnNameMatchesSameNameDifferentIds()
  {
    DataQuery q;
    q.setColumnValues(ColumnKey::Name, {QStringLiteral("Beta.las")});
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a2", "a3"}));
  }

  void columnTypeUsesEffectiveType()
  {
    DataQuery q;
    q.setColumnValues(ColumnKey::Type, {QStringLiteral("horizon")});
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a5"})); // 覆盖值可选
    q.setColumnValues(ColumnKey::Type, {QStringLiteral("seismic")});
    QCOMPARE(idsMatching(q, fixtureRows()), set({"a4"})); // a5 原型不参与
  }

  void candidatesExcludeOwnColumnKeepSelectedAndRespectOthers()
  {
    DataQuery q; // 其他条件：搜索 Beta（命中 a2/a3）；本列约束先给 {W1}。
    q.group.add({FilterDim::Search, QStringLiteral("Beta"), false});
    q.setColumnValues(ColumnKey::Entity, {QStringLiteral("W1")});
    // 候选 = 组结果行的实体值 ∪ 当前已选：a2→W1,W2；a3→无 → {W1,W2}。
    const QStringList cand = q.columnCandidates(ColumnKey::Entity, fixtureRows());
    QCOMPARE(cand, QStringList({QStringLiteral("W1"), QStringLiteral("W2")}));
    // 零命中已选值仍在候选里（可取消）。
    q.setColumnValues(ColumnKey::Entity, {QStringLiteral("W9-none")});
    QVERIFY(q.columnCandidates(ColumnKey::Entity, fixtureRows())
                .contains(QStringLiteral("W9-none")));
    QCOMPARE(idsMatching(q, fixtureRows()), set({}));
  }

  // ---- W4：状态串 v2 / legacy / 诊断 ------------------------------------
  void v2RoundTripPreservesSpecials()
  {
    DataQuery q;
    q.group.add({FilterDim::Search, QStringLiteral("!bang"), false});
    q.group.add({FilterDim::Tag, QStringLiteral("中文 标签#一&二%三+四"), true});
    q.group.add({FilterDim::Entity, QStringLiteral("~tilde~"), false});
    q.group.add({FilterDim::Regex, QStringLiteral("a~b%c"), false});
    q.group.orMode = true;
    q.activeTag = QStringLiteral("核心 标签");
    q.setColumnValues(ColumnKey::Type,
                      {QStringLiteral("well_log"), QStringLiteral("tops")});
    q.setColumnValues(ColumnKey::Entity, {QStringLiteral("W1")});
    const QueryStateParse back = parseQueryStateString(serializeQuery(q));
    QVERIFY(back.valid);
    QVERIFY(!back.legacy);
    // 逐条件对账（内容/否定/值），不以字符串相等掩盖语义丢失。
    QCOMPARE(back.query.group.conditions.size(), 4);
    QCOMPARE(back.query.group.orMode, true);
    QCOMPARE(back.query.group.conditions.at(0).value, QStringLiteral("!bang"));
    QCOMPARE(back.query.group.conditions.at(1).value,
             QStringLiteral("中文 标签#一&二%三+四"));
    QCOMPARE(back.query.group.conditions.at(1).negate, true);
    QCOMPARE(back.query.group.conditions.at(2).value, QStringLiteral("~tilde~"));
    QCOMPARE(back.query.group.conditions.at(3).value, QStringLiteral("a~b%c"));
    QCOMPARE(back.query.activeTag, QStringLiteral("核心 标签"));
    QCOMPARE(back.query.columns.size(), 2);
    QCOMPARE(back.query.columns.at(0).values,
             QStringList({QStringLiteral("tops"), QStringLiteral("well_log")}));
    QCOMPARE(back.query.columns.at(1).key, ColumnKey::Entity);
    // 语义对账：往返后的查询在夹具上产生同一 ID 集。
    QCOMPARE(idsMatching(back.query, fixtureRows()), idsMatching(q, fixtureRows()));
  }

  void v2ExplicitEmptySetRoundTrip()
  {
    DataQuery q;
    q.setColumnValues(ColumnKey::Type, {}); // 显式空集合必须保真（≠ 未设置）
    const QueryStateParse back = parseQueryStateString(serializeQuery(q));
    QVERIFY(back.valid);
    QVERIFY(back.query.column(ColumnKey::Type) != nullptr);
    QVERIFY(back.query.column(ColumnKey::Type)->values.isEmpty());
    QCOMPARE(idsMatching(back.query, fixtureRows()), set({}));
  }

  void legacyV1StringStillParsesWithDocumentedSemantics()
  {
    // 旧格式：op=or、q=、type=!seismic（! 前缀 = negate——旧语义，如实保留）。
    const QueryStateParse back = parseQueryStateString(
        QStringLiteral("paleo://dataops-filter?op=or&q=Beta&type=!seismic"));
    QVERIFY(back.valid);
    QVERIFY(back.legacy);
    QCOMPARE(back.query.group.conditions.size(), 2);
    QCOMPARE(back.query.group.conditions.at(1).negate, true);
    QCOMPARE(back.query.group.conditions.at(1).value, QStringLiteral("seismic"));
    // 手推 OR：名字含 Beta {a2,a3} ∪ 非 seismic {a1,a2,a3,a5,a6,a7} =
    // {a1,a2,a3,a5,a6,a7}。
    QCOMPARE(idsMatching(back.query, fixtureRows()),
             set({"a1", "a2", "a3", "a5", "a6", "a7"}));
    // 旧 FilterGroup API 的串在 v1 语义下与之一致。
    const FilterGroup legacy = FilterGroup::fromStateString(
        QStringLiteral("paleo://dataops-filter?op=or&q=Beta&type=!seismic"));
    QCOMPARE(legacy.conditions.size(), 2);
  }

  void badStringsReturnDiagnosticsNotSilentDrop()
  {
    const auto bad = [](const QString &s) {
      const QueryStateParse p = parseQueryStateString(s);
      return !p.valid && !p.diagnostic.isEmpty();
    };
    QVERIFY(bad(QString()));                                   // 空串
    QVERIFY(bad(QStringLiteral("not a url")));                 // 非 URL
    QVERIFY(bad(QStringLiteral("http://nope?q=1")));           // 错 scheme/host
    QVERIFY(bad(QStringLiteral("paleo://other-host?v=2")));    // 错 host
    QVERIFY(bad(QStringLiteral("paleo://dataops-filter?v=9&op=and"))); // 未知版本
    QVERIFY(bad(QStringLiteral("paleo://dataops-filter?v=2&bogus=1"))); // 未知字段
    QVERIFY(bad(QStringLiteral("paleo://dataops-filter?v=2&f=zzz~p~x"))); // 未知维度
    QVERIFY(bad(QStringLiteral("paleo://dataops-filter?v=2&f=type~p")));  // 载荷损坏
    QVERIFY(bad(QStringLiteral("paleo://dataops-filter?v=2&op=xor")));    // op 损坏
    // 诊断可读（示例两条含关键信息）。
    const QueryStateParse p = parseQueryStateString(
        QStringLiteral("paleo://dataops-filter?v=2&bogus=1"));
    QVERIFY(p.diagnostic.contains(QStringLiteral("bogus")));
  }

  void emptyQuerySerializesToValidEmpty()
  {
    DataQuery q;
    const QueryStateParse back = parseQueryStateString(serializeQuery(q));
    QVERIFY(back.valid);
    QVERIFY(back.query.isEmpty());
    QCOMPARE(idsMatching(back.query, fixtureRows()),
             set({"a1", "a2", "a3", "a4", "a5", "a6", "a7"}));
  }

  // ---- W4：预设（同一存储，v2 全量）--------------------------------------
  void presetV2RoundTripAndLegacyCompat()
  {
    DataQuery q;
    q.group.add({FilterDim::Type, QStringLiteral("tops"), false});
    q.activeTag = QStringLiteral("核心");
    q.setColumnValues(ColumnKey::Status, {QStringLiteral("DERIVED")});
    QVERIFY(saveQueryPreset(QStringLiteral("p2-预设"), q));
    const QueryStateParse loaded = loadQueryPreset(QStringLiteral("p2-预设"));
    QVERIFY(loaded.valid);
    QCOMPARE(loaded.query.group.conditions.size(), 1);
    QCOMPARE(loaded.query.activeTag, QStringLiteral("核心"));
    QCOMPARE(loaded.query.columns.size(), 1);
    // 旧入口读新预设：取条件组部分（列约束/标签需新入口）。
    const FilterGroup legacyPart = loadFilterPreset(QStringLiteral("p2-预设"));
    QCOMPARE(legacyPart.conditions.size(), 1);
    // 旧格式预设（saveFilterPreset 写 v1 串）经新入口读为 legacy。
    FilterGroup g1;
    g1.add({FilterDim::Search, QStringLiteral("Beta"), false});
    QVERIFY(saveFilterPreset(QStringLiteral("p1-旧预设"), g1));
    const QueryStateParse old = loadQueryPreset(QStringLiteral("p1-旧预设"));
    QVERIFY(old.valid);
    QVERIFY(old.legacy);
    QCOMPARE(old.query.group.conditions.size(), 1);
    // 坏内容预设（手工写坏串）→ 诊断、不替换。
    QSettings s(QStringLiteral("paleo"), QStringLiteral("paleo"));
    QVariantMap m = s.value(QStringLiteral("dataops/filterPresets")).toMap();
    m.insert(QStringLiteral("坏预设"), QStringLiteral("http://bad"));
    s.setValue(QStringLiteral("dataops/filterPresets"), m);
    const QueryStateParse broken = loadQueryPreset(QStringLiteral("坏预设"));
    QVERIFY(!broken.valid);
    QVERIFY(!broken.diagnostic.isEmpty());
    // 存储还是同一份（没有第二套配置系统）。
    QVERIFY(savedFilterNames().contains(QStringLiteral("p2-预设")));
    QVERIFY(deleteFilterPreset(QStringLiteral("p2-预设")));
    QVERIFY(deleteFilterPreset(QStringLiteral("p1-旧预设")));
  }
};

QTEST_MAIN(TestDataQuery)
#include "tst_dataquery.moc"
