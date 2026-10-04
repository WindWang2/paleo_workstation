// 层：测试壳
#include "workflow/boundaryeditrules.h"
#include "workflow/boundarysemantics.h"
#include <QtTest>

// 方向39：相界类型编辑规则——三类差异化语义的纯规则面。
//   整合接触切两侧被拒（有原因文案）、尖灭开放端可过、相变带可带渐变域、
//   表外值/负带宽如实拒绝；未分类/未知相代码中性放行（诚实面）。
class TestBoundaryEditRules : public QObject
{
    Q_OBJECT

  private slots:
    void vocabAllFourKindsActive();

    void conformableRejectsCuttingTwoFacies();
    void conformablePassesWhenSidesAgreeOrUnknown();
    void pinchoutAcceptsOpenRingOthersReject();
    void transitionBandOnlyForFaciesChange();
    void unknownKindAndNegativeWidthRejected();
    void reasonTextCarriesFrozenVocab();
};

void TestBoundaryEditRules::vocabAllFourKindsActive()
{
    QCOMPARE(BoundarySemantics::activeKinds(), BoundarySemantics::kinds());
    QVERIFY(BoundarySemantics::isKnownKind(QStringLiteral("conformable")));
    QVERIFY(BoundarySemantics::isKnownKind(QStringLiteral("pinchout")));
    QVERIFY(BoundarySemantics::isKnownKind(QStringLiteral("facies_change")));
    QVERIFY(BoundarySemantics::isKnownKind(QStringLiteral("fault_cut")));
    QVERIFY(!BoundarySemantics::isKnownKind(QStringLiteral("erosion")));
    QVERIFY(!BoundarySemantics::isKnownKind(QString()));
}

void TestBoundaryEditRules::conformableRejectsCuttingTwoFacies()
{
    BoundarySemantics::BoundaryEditFacts facts;
    facts.kind = QStringLiteral("conformable");
    facts.selfFaciesCode = 1;
    facts.hasAdjacent = true;
    facts.adjacentFaciesCode = 2;
    facts.adjacentFaciesDiffers = true;
    const auto verdict = BoundarySemantics::checkKindAssignment(facts);
    QVERIFY(!verdict.accepted);
    QVERIFY(!verdict.reason.isEmpty());

    // 同侧相（不切割）可过：邻接相代码与本要素相同。
    BoundarySemantics::BoundaryEditFacts agree = facts;
    agree.adjacentFaciesCode = 1;
    agree.adjacentFaciesDiffers = false;
    QVERIFY(BoundarySemantics::checkKindAssignment(agree).accepted);

    // 其余三类不设此门（尖灭/相变/断层切割两侧相异是常态语义）。
    for (const char *kind : { "pinchout", "facies_change", "fault_cut" })
    {
        BoundarySemantics::BoundaryEditFacts other = facts;
        other.kind = QString::fromLatin1(kind);
        QVERIFY2(BoundarySemantics::checkKindAssignment(other).accepted, kind);
    }
}

void TestBoundaryEditRules::conformablePassesWhenSidesAgreeOrUnknown()
{
    // 相代码未知（<0）不参与拒绝——诚实中性，不能凭未知数据拦人。
    BoundarySemantics::BoundaryEditFacts unknownSelf;
    unknownSelf.kind = QStringLiteral("conformable");
    unknownSelf.selfFaciesCode = -1;
    unknownSelf.hasAdjacent = true;
    unknownSelf.adjacentFaciesCode = 2;
    unknownSelf.adjacentFaciesDiffers = false; // 侧相未知 → 不判差异
    QVERIFY(BoundarySemantics::checkKindAssignment(unknownSelf).accepted);

    // 无邻接要素（孤岛）也没有「两侧」可切。
    BoundarySemantics::BoundaryEditFacts isolated;
    isolated.kind = QStringLiteral("conformable");
    isolated.selfFaciesCode = 1;
    isolated.hasAdjacent = false;
    QVERIFY(BoundarySemantics::checkKindAssignment(isolated).accepted);

    // 未分类恒可过。
    BoundarySemantics::BoundaryEditFacts unclassified;
    QVERIFY(BoundarySemantics::checkKindAssignment(unclassified).accepted);
}

void TestBoundaryEditRules::pinchoutAcceptsOpenRingOthersReject()
{
    BoundarySemantics::BoundaryEditFacts open;
    open.ringClosed = false;

    open.kind = QStringLiteral("pinchout");
    QVERIFY(BoundarySemantics::checkRingClosure(open).accepted); // 尖灭开放端可过

    open.kind = QString();
    QVERIFY(BoundarySemantics::checkRingClosure(open).accepted); // 未分类不判

    for (const char *kind : { "conformable", "facies_change", "fault_cut" })
    {
        open.kind = QString::fromLatin1(kind);
        const auto verdict = BoundarySemantics::checkRingClosure(open);
        QVERIFY2(!verdict.accepted, kind);
        QVERIFY2(!verdict.reason.isEmpty(), kind);
    }

    // 闭合环任何类型都可过。
    BoundarySemantics::BoundaryEditFacts closed;
    closed.kind = QStringLiteral("conformable");
    closed.ringClosed = true;
    QVERIFY(BoundarySemantics::checkRingClosure(closed).accepted);
}

void TestBoundaryEditRules::transitionBandOnlyForFaciesChange()
{
    // 相变带渐变域：宽度 > 0 且类型为相变 → 可过。
    BoundarySemantics::BoundaryEditFacts band;
    band.kind = QStringLiteral("facies_change");
    band.transitionWidth = 250.5;
    QVERIFY(BoundarySemantics::checkTransitionBand(band).accepted);

    // 带域随其它类型/未分类提交 → 拒绝。
    for (const char *kind : { "conformable", "pinchout", "fault_cut" })
    {
        BoundarySemantics::BoundaryEditFacts other = band;
        other.kind = QString::fromLatin1(kind);
        const auto verdict = BoundarySemantics::checkTransitionBand(other);
        QVERIFY2(!verdict.accepted, kind);
        QVERIFY2(!verdict.reason.isEmpty(), kind);
    }
    BoundarySemantics::BoundaryEditFacts noKind = band;
    noKind.kind = QString();
    QVERIFY(!BoundarySemantics::checkTransitionBand(noKind).accepted);

    // 无带（0）任何类型可过；负带宽拒绝。
    BoundarySemantics::BoundaryEditFacts zero = band;
    zero.transitionWidth = 0;
    zero.kind = QStringLiteral("conformable");
    QVERIFY(BoundarySemantics::checkTransitionBand(zero).accepted);
    BoundarySemantics::BoundaryEditFacts negative = band;
    negative.transitionWidth = -1;
    QVERIFY(!BoundarySemantics::checkTransitionBand(negative).accepted);
}

void TestBoundaryEditRules::unknownKindAndNegativeWidthRejected()
{
    BoundarySemantics::BoundaryEditFacts facts;
    facts.kind = QStringLiteral("erosion_surface");
    const auto verdict = BoundarySemantics::checkKindAssignment(facts);
    QVERIFY(!verdict.accepted);
    QVERIFY(verdict.reason.contains(QStringLiteral("erosion_surface")));
    QVERIFY(verdict.reason.contains(QStringLiteral("整合接触")));
}

void TestBoundaryEditRules::reasonTextCarriesFrozenVocab()
{
    // 拒绝文案含冻结词面（供 UI/statusLabel 直出）与证据值。
    BoundarySemantics::BoundaryEditFacts cut;
    cut.kind = QStringLiteral("conformable");
    cut.selfFaciesCode = 1;
    cut.adjacentFaciesCode = 2;
    cut.adjacentFaciesDiffers = true;
    const auto cutVerdict = BoundarySemantics::checkKindAssignment(cut);
    QVERIFY(cutVerdict.reason.contains(QStringLiteral("整合接触")));
    QVERIFY(cutVerdict.reason.contains(QStringLiteral("相变")));
    QVERIFY(cutVerdict.reason.contains('1'));
    QVERIFY(cutVerdict.reason.contains('2'));

    BoundarySemantics::BoundaryEditFacts band;
    band.kind = QStringLiteral("pinchout");
    band.transitionWidth = 10;
    const auto bandVerdict = BoundarySemantics::checkTransitionBand(band);
    QVERIFY(bandVerdict.reason.contains(QStringLiteral("尖灭")));
    QVERIFY(bandVerdict.reason.contains(QStringLiteral("相变")));
}

QTEST_APPLESS_MAIN(TestBoundaryEditRules)
#include "tst_boundaryeditrules.moc"
