// 层：测试壳
// goal/fault-interpretation — FaultSet 域模型测试：
// 实体 CRUD/命名去重、断层棒增删与剖面查询、断层-层位切割独立存取
// （Oracle #4）、JSON 序列化往返、坏 JSON 拒绝。
#include <QtTest>

#include "../src/domain/faultset.h"

using namespace paleo::fault;

namespace {
FaultStick makeStick(const QString &id, FaultSectionRef::Kind kind, int index,
                     const QString &pathId = QString())
{
    FaultStick s;
    s.id = id;
    s.section.kind = kind;
    s.section.index = index;
    s.section.pathId = pathId;
    s.section.displayName = kind == FaultSectionRef::Inline
        ? QStringLiteral("IL %1").arg(index)
        : (kind == FaultSectionRef::Xline ? QStringLiteral("XL %1").arg(index)
                                          : QStringLiteral("任意线 %1").arg(pathId));
    s.points = {{0.0, 100.0}, {0.5, 300.0}, {1.0, 500.0}};
    s.interpreter = QStringLiteral("解释员A");
    return s;
}

FaultHorizonCut makeCut(const QString &horizon, FaultHangingSide side = FaultHangingSide::Unknown)
{
    FaultHorizonCut c;
    c.horizon = horizon;
    c.wkt = QStringLiteral("Polygon ((0 0, 1 0, 1 1, 0 1, 0 0))");
    c.hangingSide = side;
    return c;
}
} // namespace

class TestFaultSet : public QObject
{
    Q_OBJECT

private slots:
    void addFaultAssignsUniqueIdsAndNames();
    void renameRejectsCollision();
    void removeAndVisibility();
    void stickCrudAndSectionQuery();
    void cutsPerHorizonAreIndependent();
    void jsonRoundTrip();
    void rejectsMalformedJson();
    void insertFaultRestoresWithCounters();
};

void TestFaultSet::addFaultAssignsUniqueIdsAndNames()
{
    FaultSet set;
    const QString f1 = set.addFault(QStringLiteral("F"));
    const QString f2 = set.addFault(QStringLiteral("F"));
    QCOMPARE(f1, QStringLiteral("f-1"));
    QCOMPARE(f2, QStringLiteral("f-2"));
    QCOMPARE(set.faultById(f1)->name, QStringLiteral("F"));
    QCOMPARE(set.faultById(f2)->name, QStringLiteral("F2")); // 撞名自动去重
    QVERIFY(set.faultCount() == 2);
}

void TestFaultSet::renameRejectsCollision()
{
    FaultSet set;
    const QString f1 = set.addFault(QStringLiteral("F1"));
    const QString f2 = set.addFault(QStringLiteral("F2"));
    QVERIFY(!set.renameFault(f2, QStringLiteral("F1"))); // 撞名拒绝
    QCOMPARE(set.faultById(f2)->name, QStringLiteral("F2"));
    QVERIFY(set.renameFault(f2, QStringLiteral("断层A")));
    QCOMPARE(set.faultById(f2)->name, QStringLiteral("断层A"));
    QVERIFY(!set.renameFault(QStringLiteral("f-404"), QStringLiteral("X"))); // 不存在
}

void TestFaultSet::removeAndVisibility()
{
    FaultSet set;
    const QString f1 = set.addFault(QStringLiteral("F1"));
    set.setFaultVisible(f1, false);
    QVERIFY(!set.faultById(f1)->visible);
    QVERIFY(set.setFaultVisible(f1, true));
    QVERIFY(set.faultById(f1)->visible);
    QVERIFY(set.removeFault(f1));
    QVERIFY(set.faultById(f1) == nullptr);
    QVERIFY(!set.removeFault(f1)); // 已删
}

void TestFaultSet::stickCrudAndSectionQuery()
{
    FaultSet set;
    const QString f1 = set.addFault(QStringLiteral("F1"));
    const QString f2 = set.addFault(QStringLiteral("F2"));
    set.setFaultVisible(f2, false); // 隐藏断层的棒不进剖面查询

    QString assigned;
    QVERIFY(set.addStick(f1, makeStick(QString(), FaultSectionRef::Inline, 120), &assigned));
    QVERIFY(!assigned.isEmpty()); // 空 id 自动分配
    const QString stickId2 = QStringLiteral("s-manual");
    QVERIFY(set.addStick(f1, makeStick(stickId2, FaultSectionRef::Arbitrary, 0, "100,200;101,201")));
    QVERIFY(!set.addStick(QStringLiteral("f-404"), makeStick(QString(), FaultSectionRef::Inline, 1)));

    FaultSectionRef ref;
    ref.kind = FaultSectionRef::Inline;
    ref.index = 120;
    const auto sticks = set.sticksForSection(ref);
    QCOMPARE(sticks.size(), 1);
    QCOMPARE(sticks.first().first, f1);
    QCOMPARE(sticks.first().second.twtMinMs(), 100.0);
    QCOMPARE(sticks.first().second.twtMaxMs(), 500.0);

    FaultSectionRef arb;
    arb.kind = FaultSectionRef::Arbitrary;
    arb.pathId = "100,200;101,201";
    QCOMPARE(set.sticksForSection(arb).size(), 1);
    arb.pathId = "other"; // 路径不同 → 不同剖面身份
    QCOMPARE(set.sticksForSection(arb).size(), 0);

    QVERIFY(set.removeStick(f1, stickId2));
    QVERIFY(!set.removeStick(f1, stickId2)); // 已删
}

void TestFaultSet::cutsPerHorizonAreIndependent()
{
    // Oracle #4：同一断层对多层位的关系独立存取。
    FaultSet set;
    const QString f1 = set.addFault(QStringLiteral("F1"));
    QVERIFY(set.setCut(f1, makeCut(QStringLiteral("H1"), FaultHangingSide::Left)));
    QVERIFY(set.setCut(f1, makeCut(QStringLiteral("H2"), FaultHangingSide::Right)));

    QCOMPARE(set.faultById(f1)->cuts.size(), 2);
    QCOMPARE(set.cut(f1, "H1")->hangingSide, FaultHangingSide::Left);
    QCOMPARE(set.cut(f1, "H2")->hangingSide, FaultHangingSide::Right);

    // 改 H1 不影响 H2；H2 的 WKT 也换掉以证明各存各的
    FaultHorizonCut h1b = makeCut(QStringLiteral("H1"), FaultHangingSide::Right);
    h1b.wkt = QStringLiteral("Polygon ((10 10, 11 10, 11 11, 10 11, 10 10))");
    QVERIFY(set.setCut(f1, h1b));
    QCOMPARE(set.cut(f1, "H1")->wkt, h1b.wkt);
    QCOMPARE(set.cut(f1, "H1")->hangingSide, FaultHangingSide::Right);
    QCOMPARE(set.cut(f1, "H2")->hangingSide, FaultHangingSide::Right); // H2 不受影响
    QCOMPARE(set.faultById(f1)->cuts.size(), 2); // 同层位替换不增行

    QVERIFY(set.removeCut(f1, "H1"));
    QVERIFY(set.cut(f1, "H1") == nullptr);
    QVERIFY(set.cut(f1, "H2") != nullptr); // H2 独立存活
    QVERIFY(!set.removeCut(f1, "H404"));
}

void TestFaultSet::jsonRoundTrip()
{
    FaultSet set;
    const QString f1 = set.addFault(QStringLiteral("F1"), QStringLiteral("解释员A"));
    set.addStick(f1, makeStick(QString(), FaultSectionRef::Inline, 120));
    set.addStick(f1, makeStick(QString(), FaultSectionRef::Arbitrary, 0, "100,200;101,201"));
    set.setCut(f1, makeCut(QStringLiteral("H1"), FaultHangingSide::Left));
    set.setFaultVisible(f1, false);

    const QByteArray json = set.toJson();

    FaultSet back;
    QString err;
    QVERIFY2(back.fromJson(json, &err), qPrintable(err));
    QCOMPARE(back.faultCount(), 1);
    const Fault *f = back.faultById(f1);
    QVERIFY(f != nullptr);
    QCOMPARE(f->name, QStringLiteral("F1"));
    QCOMPARE(f->interpreter, QStringLiteral("解释员A"));
    QVERIFY(!f->visible);
    QCOMPARE(f->sticks.size(), 2);
    QCOMPARE(f->sticks.at(0), set.faultById(f1)->sticks.at(0));
    QCOMPARE(f->sticks.at(1), set.faultById(f1)->sticks.at(1));
    QVERIFY(f->cuts.at(0) == *set.cut(f1, "H1"));

    // 解析后新分配 id 不与已恢复 id 冲突
    const QString f2 = back.addFault(QStringLiteral("F"));
    QVERIFY(f2 != f1);
    QCOMPARE(back.faultById(f2)->name, QStringLiteral("F")); // "F" 空闲直接用
}

void TestFaultSet::rejectsMalformedJson()
{
    FaultSet set;
    QString err;
    QVERIFY(!set.fromJson(QByteArray("not json"), &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!set.fromJson(QByteArray("{\"format\":\"paleo-faultset\"}"), &err)); // 缺 faults
    QVERIFY(!set.fromJson(QByteArray("{\"faults\":[{}]}"), &err)); // 元素缺 id/name
    QCOMPARE(set.faultCount(), 0); // 失败不变更
}

void TestFaultSet::insertFaultRestoresWithCounters()
{
    FaultSet set;
    const QString f1 = set.addFault(QStringLiteral("F1"));
    Fault snapshot = *set.faultById(f1);
    set.addStick(f1, makeStick(QStringLiteral("s-9"), FaultSectionRef::Inline, 10), nullptr);
    set.removeFault(f1);

    set.insertFault(snapshot); // undo 重放路径：原样恢复
    QCOMPARE(set.faultCount(), 1);
    QVERIFY(set.faultById(f1) != nullptr);

    // 计数器上抬：新分配不与恢复的 id 冲突
    const QString f2 = set.addFault(QStringLiteral("F"));
    QVERIFY(f2 != f1);
    QString assigned;
    set.addStick(f2, makeStick(QString(), FaultSectionRef::Inline, 11), &assigned);
    QVERIFY(assigned != QStringLiteral("s-9"));
}

QTEST_MAIN(TestFaultSet)
#include "tst_faultset.moc"
