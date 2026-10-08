// 层：测试壳
// goal/fault-surface — 断棒成面、断距、剖面交线、体域阻断、性能。
#include <QtTest>
#include <QElapsedTimer>
#include <QSet>
#include <QtMath>

#include <cmath>
#include <limits>
#include <vector>

#include "../src/algorithms/faultsurface/faultsurface.h"
#include "../src/algorithms/stratgrid/propfill.h"
#include "../src/algorithms/stratgrid/stratgrid.h"

using namespace paleo::fault;
using namespace paleo::faultsurf;

namespace {

SurveyFrame testFrame()
{
    SurveyFrame frame;
    frame.inlineStepX = 0;
    frame.inlineStepY = 100;
    frame.xlineStepX = 100;
    frame.xlineStepY = 0;
    frame.inlineMin = 0;
    frame.inlineMax = 10;
    frame.xlineMin = 0;
    frame.xlineMax = 10;
    frame.zScale = 1;
    return frame;
}

double tan30()
{
    return std::tan(qDegreesToRadians(30.0));
}

FaultStick inlineStick(int index, const QString &id, int nPoints, double u0 = 0, double u1 = 1)
{
    FaultStick stick;
    stick.id = id;
    stick.section.kind = FaultSectionRef::Inline;
    stick.section.index = index;
    stick.section.displayName = QStringLiteral("IL %1").arg(index);
    const int n = std::max(2, nPoints);
    for (int i = 0; i < n; ++i) {
        const double u = n == 1 ? u0 : u0 + (u1 - u0) * (double(i) / double(n - 1));
        const double x = u * 1000.0;
        stick.points.append({u, x * tan30()});
    }
    return stick;
}

bool near(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

bool angNear(double a, double b, double tol)
{
    double d = std::fabs(a - b);
    while (d > 180.0)
        d = 360.0 - d;
    return d <= tol;
}

Fault planarFault(const QVector<int> &inlines, const QVector<int> &counts, const QStringList &ids)
{
    Fault fault;
    fault.id = QStringLiteral("f-1");
    fault.name = QStringLiteral("F1");
    for (int i = 0; i < inlines.size(); ++i)
        fault.sticks.append(inlineStick(inlines.at(i), ids.at(i), counts.at(i)));
    return fault;
}

} // namespace

class TestFaultSurface : public QObject
{
    Q_OBJECT

private slots:
    void planarDipAndTopology();
    void unequalAndShuffledSectionsKeepStickIdentity();
    void singleSectionIsRejected();
    void branchingAndMixedDomainFail();
    void constantSlipAlongIntersection();
    void sectionCutOnUnpickedInline();
    void partialDepthMeshDoesNotCurtainTheColumn();
    void segmentMeshIndexMatchesLinearScan();
    void segmentMeshIndexRejectsDegenerateInput();
    void hundredByTwoHundredUnderThreeSeconds();
    void polylineCurtainCut();
};

void TestFaultSurface::planarDipAndTopology()
{
    const Fault fault = planarFault({0, 1, 2}, {2, 2, 2}, {QStringLiteral("s-a"), QStringLiteral("s-b"), QStringLiteral("s-c")});
    const SurfaceBuildResult built = buildFaultSurface(fault, testFrame());
    QVERIFY2(built.ok(), qPrintable(built.message));

    Attitude att;
    QVERIFY(surfaceAttitude(built.mesh, &att));
    QVERIFY2(near(att.dipDeg, 30.0, 0.5), qPrintable(QString::number(att.dipDeg)));
    QVERIFY2(angNear(att.dipAzimuthDeg, 90.0, 0.5), qPrintable(QString::number(att.dipAzimuthDeg)));
    QVERIFY2(angNear(att.strikeDeg, 0.0, 0.5), qPrintable(QString::number(att.strikeDeg)));

    const MeshTopology topo = validateMeshTopology(built.mesh);
    QVERIFY2(topo.ok, qPrintable(topo.message));
    QCOMPARE(topo.degenerate, 0);
    QCOMPARE(topo.nonManifoldEdges, 0);
    QVERIFY(topo.boundaryEdges > 0);

    for (const FaultSurfaceVertex &v : built.mesh.vertices) {
        QVERIFY2(near(v.z, v.x * tan30(), 1e-6), qPrintable(QStringLiteral("z=%1 x=%2").arg(v.z).arg(v.x)));
        const FaultStick *stick = fault.stickById(v.stickId);
        QVERIFY(stick != nullptr);
        QVERIFY(v.pointIndex >= 0 && v.pointIndex < stick->points.size());
        const double u = stick->points.at(v.pointIndex).first;
        const double sample = stick->points.at(v.pointIndex).second;
        QVERIFY(near(v.x, u * 1000.0, 1e-6));
        QVERIFY(near(v.y, stick->section.index * 100.0, 1e-6));
        QVERIFY(near(v.z, sample, 1e-6));
    }

    const FaultSurfaceVertex &mid = built.mesh.vertices.at(built.mesh.vertices.size() / 2);
    Attitude local;
    QVERIFY(attitudeAt(built.mesh, mid.x, mid.y, mid.z, &local));
    QVERIFY(near(local.dipDeg, 30.0, 0.5));
}

void TestFaultSurface::unequalAndShuffledSectionsKeepStickIdentity()
{
    Fault fault;
    fault.id = QStringLiteral("f-1");
    fault.name = QStringLiteral("F1");
    fault.sticks.append(inlineStick(2, QStringLiteral("s-late"), 3));
    fault.sticks.append(inlineStick(0, QStringLiteral("s-early"), 5));
    fault.sticks.append(inlineStick(1, QStringLiteral("s-mid"), 4));
    const SurfaceBuildResult built = buildFaultSurface(fault, testFrame());
    QVERIFY2(built.ok(), qPrintable(built.message));
    QCOMPARE(built.mesh.vertices.size(), 3 + 5 + 4);
    QCOMPARE(built.mesh.stickOrder, QStringList({QStringLiteral("s-early"), QStringLiteral("s-mid"), QStringLiteral("s-late")}));
    QSet<QString> seen;
    for (const FaultSurfaceVertex &v : built.mesh.vertices) {
        seen.insert(v.stickId);
        const FaultStick *stick = fault.stickById(v.stickId);
        QVERIFY(stick != nullptr);
        QVERIFY(v.pointIndex >= 0 && v.pointIndex < stick->points.size());
    }
    QCOMPARE(seen.size(), 3);
    QVERIFY(validateMeshTopology(built.mesh).ok);
}

void TestFaultSurface::singleSectionIsRejected()
{
    Fault fault;
    fault.id = QStringLiteral("f-1");
    fault.name = QStringLiteral("F1");
    fault.sticks.append(inlineStick(4, QStringLiteral("s-only"), 6));
    const SurfaceBuildResult built = buildFaultSurface(fault, testFrame());
    QCOMPARE(built.status, SurfaceBuildStatus::TooFewSections);
    QVERIFY(built.mesh.isEmpty());
    QVERIFY(built.message.contains(QStringLiteral("单剖面")));
}

void TestFaultSurface::branchingAndMixedDomainFail()
{
    Fault branch;
    branch.id = QStringLiteral("f-1");
    branch.name = QStringLiteral("F1");
    branch.sticks.append(inlineStick(0, QStringLiteral("s-1"), 3, 0.0, 0.7));
    branch.sticks.append(inlineStick(0, QStringLiteral("s-2"), 3, 0.3, 1.0));
    branch.sticks.append(inlineStick(2, QStringLiteral("s-3"), 2));
    const SurfaceBuildResult branched = buildFaultSurface(branch, testFrame());
    QCOMPARE(branched.status, SurfaceBuildStatus::Branching);
    QVERIFY(branched.mesh.isEmpty());

    Fault mixed = planarFault({0, 2}, {2, 2}, {QStringLiteral("s-a"), QStringLiteral("s-b")});
    mixed.sticks[1].verticalDomain = VerticalDomain::Depth;
    const SurfaceBuildResult rejected = buildFaultSurface(mixed, testFrame());
    QCOMPARE(rejected.status, SurfaceBuildStatus::MixedDomain);
    QVERIFY(rejected.mesh.isEmpty());

    Fault turn;
    turn.id = QStringLiteral("f-1");
    turn.name = QStringLiteral("F1");
    turn.sticks.append(inlineStick(0, QStringLiteral("s-il0"), 2));
    turn.sticks.append(inlineStick(1, QStringLiteral("s-il1"), 2));
    FaultStick xl;
    xl.id = QStringLiteral("s-xl");
    xl.section.kind = FaultSectionRef::Xline;
    xl.section.index = 0;
    xl.points.append({0.0, 0.0});
    xl.points.append({1.0, 100.0});
    turn.sticks.append(xl);
    const SurfaceBuildResult broken = buildFaultSurface(turn, testFrame());
    QCOMPARE(broken.status, SurfaceBuildStatus::DirectionBreak);
}

void TestFaultSurface::constantSlipAlongIntersection()
{
    const Fault fault = planarFault({0, 1, 2}, {2, 2, 2}, {QStringLiteral("s-a"), QStringLiteral("s-b"), QStringLiteral("s-c")});
    const SurfaceBuildResult built = buildFaultSurface(fault, testFrame());
    QVERIFY2(built.ok(), qPrintable(built.message));
    HorizonPlane foot;
    foot.nz = 1;
    foot.d = 100;
    HorizonPlane hang;
    hang.nz = 1;
    hang.d = 200;
    const SlipCurve curve = measureSlip(built.mesh, foot, hang);
    QVERIFY2(curve.ok(), qPrintable(curve.message));
    const double throwTrue = 100.0;
    const double heaveTrue = throwTrue / tan30();
    QVERIFY2(near(curve.throwMin, throwTrue, 0.05), qPrintable(QString::number(curve.throwMin)));
    QVERIFY2(near(curve.throwMax, throwTrue, 0.05), qPrintable(QString::number(curve.throwMax)));
    QVERIFY2(near(curve.heaveMin, heaveTrue, 0.05), qPrintable(QString::number(curve.heaveMin)));
    QVERIFY2(near(curve.heaveMax, heaveTrue, 0.05), qPrintable(QString::number(curve.heaveMax)));
    QVERIFY(curve.samples.size() >= 2);
    QVERIFY2(near(curve.length, 200.0, 1.0), qPrintable(QString::number(curve.length)));
    QVERIFY(curve.samples.last().along > curve.samples.first().along);
    for (const SlipSample &sample : curve.samples) {
        QVERIFY(near(sample.throwZ, throwTrue, 0.05));
        QVERIFY(near(sample.heave, heaveTrue, 0.05));
    }
}

void TestFaultSurface::sectionCutOnUnpickedInline()
{
    const Fault fault = planarFault({0, 2}, {2, 2}, {QStringLiteral("s-a"), QStringLiteral("s-b")});
    const SurfaceBuildResult built = buildFaultSurface(fault, testFrame());
    QVERIFY2(built.ok(), qPrintable(built.message));
    FaultSectionRef query;
    query.kind = FaultSectionRef::Inline;
    query.index = 1;
    const SectionCut cut = intersectSurfaceWithSection(built.mesh, testFrame(), query);
    QVERIFY2(cut.ok(), qPrintable(cut.message));
    QVERIFY(cut.hits.size() >= 2);
    const SectionHit *closest = &cut.hits.first();
    for (const SectionHit &hit : cut.hits) {
        if (std::fabs(hit.traceFrac - 0.5) < std::fabs(closest->traceFrac - 0.5))
            closest = &hit;
    }
    QVERIFY2(near(closest->traceFrac, 0.5, 1e-6), qPrintable(QString::number(closest->traceFrac)));
    QVERIFY2(near(closest->sample, 500.0 * tan30(), 1e-4), qPrintable(QString::number(closest->sample)));
    QVERIFY2(near(closest->y, 100.0, 1e-4), qPrintable(QString::number(closest->y)));
    QVERIFY2(near(closest->z, closest->x * tan30(), 1e-4),
             qPrintable(QStringLiteral("x=%1 z=%2").arg(closest->x).arg(closest->z)));
}

void TestFaultSurface::partialDepthMeshDoesNotCurtainTheColumn()
{
    using paleo::stratgrid::FaultSegment;
    using paleo::stratgrid::FaultTriangle;
    using paleo::stratgrid::PropertyVolume;
    using paleo::stratgrid::Seed;
    using paleo::stratgrid::ZoneGrid;
    using paleo::stratgrid::fillIdw;

    ZoneGrid grid;
    grid.ni = 6;
    grid.nj = 1;
    grid.nk = 4;
    grid.dx = 10;
    grid.dy = 10;
    grid.topZ.assign(6, 0.f);
    grid.botZ.assign(6, 40.f);
    grid.live.assign(6, 1);
    grid.liveColumns = 6;
    const std::vector<Seed> seeds{Seed{0, 0, 0, 4.0}};

    std::vector<FaultSegment> curtain{FaultSegment{20, -1, 20, 11}};
    PropertyVolume plan;
    QVERIFY(fillIdw(grid, seeds, curtain, 2.0, &plan));
    QVERIFY(!std::isfinite(plan.values[static_cast<std::size_t>(grid.cellIndex(3, 0, 0))]));

    const std::vector<FaultTriangle> partial{
        FaultTriangle{20, -1, 20, 20, 11, 20, 20, -1, 40},
        FaultTriangle{20, 11, 40, 20, 11, 20, 20, -1, 40},
    };
    PropertyVolume volume;
    QVERIFY(fillIdw(grid, seeds, partial, 2.0, &volume));
    const float shallow = volume.values[static_cast<std::size_t>(grid.cellIndex(3, 0, 0))];
    QVERIFY2(std::isfinite(shallow), qPrintable(QString::number(shallow)));
    QCOMPARE(volume.cellBlock[static_cast<std::size_t>(grid.cellIndex(0, 0, 0))],
             volume.cellBlock[static_cast<std::size_t>(grid.cellIndex(3, 0, 0))]);

    const std::vector<FaultTriangle> full{
        FaultTriangle{20, -1, -1, 20, 11, -1, 20, -1, 80},
        FaultTriangle{20, 11, 80, 20, 11, -1, 20, -1, 80},
    };
    PropertyVolume sealed;
    QVERIFY(fillIdw(grid, seeds, full, 2.0, &sealed));
    QVERIFY(!std::isfinite(sealed.values[static_cast<std::size_t>(grid.cellIndex(3, 0, 0))]));
    QVERIFY(sealed.cellBlock[static_cast<std::size_t>(grid.cellIndex(0, 0, 0))] !=
            sealed.cellBlock[static_cast<std::size_t>(grid.cellIndex(3, 0, 0))]);
}

void TestFaultSurface::segmentMeshIndexMatchesLinearScan()
{
    // #234 项 1：空间索引与线性扫描必须逐位一致（AABB 预筛是保守必要条件，
    // 命中蕴含线段与三角形 AABB 均相交——剔除永不误杀真实命中）。
    std::vector<Triangle3> tris;
    unsigned state = 12345;
    const auto nextUnit = [&state]() {
        state = state * 1103515245u + 12345u;
        return double((state >> 8) & 0xFFFFFFu) / double(0xFFFFFFu);
    };
    // 8×8 倾斜三角网（z 抖动 ±2.5），覆盖 [0,80]×[0,80]×[0,5]。
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) {
            const double x0 = c * 10.0, x1 = x0 + 10.0;
            const double y0 = r * 10.0, y1 = y0 + 10.0;
            const double z00 = 2.5 * nextUnit(), z10 = 2.5 * nextUnit();
            const double z01 = 2.5 * nextUnit(), z11 = 2.5 * nextUnit();
            tris.push_back(Triangle3{x0, y0, z00, x1, y0, z10, x0, y1, z01});
            tris.push_back(Triangle3{x1, y0, z10, x1, y1, z11, x0, y1, z01});
        }
    SegmentMeshIndex index;
    index.build(tris);
    QVERIFY(index.usable());

    bool consistent = true;
    QString where;
    const auto check = [&](double x1, double y1, double z1, double x2, double y2, double z2) {
        if (!consistent)
            return;
        const bool linear = segmentIntersectsTriangles(x1, y1, z1, x2, y2, z2, tris);
        const bool indexed = index.segmentIntersects(x1, y1, z1, x2, y2, z2);
        if (indexed != linear) {
            consistent = false;
            where = QStringLiteral("(%1,%2,%3)->(%4,%5,%6) linear=%7 indexed=%8")
                        .arg(x1).arg(y1).arg(z1).arg(x2).arg(y2).arg(z2)
                        .arg(linear).arg(indexed);
        }
    };
    // 短步（模拟 fillIdw 六邻 BFS 边）+ 长斜穿段，密扫全空间。
    for (double z = -5.0; z <= 30.0; z += 5.0)
        for (double y = -5.0; y <= 85.0; y += 5.0)
            for (double x = -5.0; x <= 85.0; x += 5.0) {
                check(x, y, z, x + 5.0, y, z);
                check(x, y, z, x, y + 5.0, z);
                check(x, y, z, x, y, z + 5.0);
                check(x, y, z, x + 30.0, y + 17.0, z + 3.0);
            }
    QVERIFY2(consistent, qPrintable(where));
}

void TestFaultSurface::segmentMeshIndexRejectsDegenerateInput()
{
    SegmentMeshIndex empty;
    empty.build({});
    QVERIFY(!empty.usable()); // 空 mesh：调用方回退线性扫描（空集必不命中）

    SegmentMeshIndex point;
    point.build({Triangle3{1, 1, 1, 1, 1, 1, 1, 1, 1}});
    QVERIFY(!point.usable()); // 全体共点：零体积格架建不起来，回退线性扫描

    SegmentMeshIndex nonFinite;
    nonFinite.build({Triangle3{0, 0, 0, 1, 0, 0, 0, 1, std::numeric_limits<double>::quiet_NaN()}});
    QVERIFY(!nonFinite.usable()); // 全非有限三角形：索引剔除后为空，回退（线性版对 NaN 亦必不命中）
}

void TestFaultSurface::hundredByTwoHundredUnderThreeSeconds()
{
    Fault fault;
    fault.id = QStringLiteral("f-1");
    fault.name = QStringLiteral("F1");
    constexpr int kSections = 100;
    constexpr int kPoints = 200;
    for (int i = 0; i < kSections; ++i)
        fault.sticks.append(inlineStick(i, QStringLiteral("s-%1").arg(i), kPoints));
    QElapsedTimer timer;
    timer.start();
    const SurfaceBuildResult built = buildFaultSurface(fault, testFrame());
    const qint64 elapsed = timer.elapsed();
    QVERIFY2(built.ok(), qPrintable(built.message));
    QVERIFY2(elapsed < 3000, qPrintable(QStringLiteral("%1 ms").arg(elapsed)));
    QCOMPARE(built.mesh.vertices.size(), kSections * kPoints);
    QVERIFY(built.mesh.triangles.size() > 0);
    QVERIFY(built.mesh.triangles.size() < 2 * kSections * kPoints);
    const MeshTopology topo = validateMeshTopology(built.mesh);
    QVERIFY2(topo.ok, qPrintable(topo.message));
    qInfo("fault-surface perf: %lld ms for %d x %d (budget 3000 ms, ratio %f)",
          static_cast<long long>(elapsed), kSections, kPoints, double(elapsed) / 3000.0);
}

// ---- 任意折线 curtain 求交（连井剖面断层投绘）----
// 倾斜平面 mesh（z = x/10，x∈[0,100]、y∈[-50,50]），竖直 curtain 沿
// x=50 的折线切过：交点 z 全为 5，along 随折线累计长分数推进。
void TestFaultSurface::polylineCurtainCut()
{
    paleo::fault::FaultSurfaceMesh mesh;
    // 四角两三角形：A(0,-50,0) B(100,-50,10) C(100,50,10) D(0,50,0)。
    mesh.vertices = {
        {0, -50, 0, QStringLiteral("s1"), 0},
        {100, -50, 10, QStringLiteral("s1"), 1},
        {100, 50, 10, QStringLiteral("s2"), 0},
        {0, 50, 0, QStringLiteral("s2"), 1},
    };
    mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
    QVector<QPair<double, double>> path;
    path << qMakePair(50.0, -100.0) << qMakePair(50.0, 100.0);

    const SectionCut cut = intersectSurfaceWithPolyline(mesh, path);
    QVERIFY2(cut.ok(), qPrintable(cut.message));
    QVERIFY(cut.hits.size() >= 2);
    QVERIFY(cut.hits.first().traceFrac <= cut.hits.last().traceFrac); // 排序
    // 折线总长 200，起点 y=-100：y=-50 → 0.25；y=50 → 0.75。
    QCOMPARE(qRound(cut.hits.first().traceFrac * 10000.0), 2500);
    QCOMPARE(qRound(cut.hits.last().traceFrac * 10000.0), 7500);
    for (const SectionHit &h : cut.hits)
        QCOMPARE(qRound(h.z * 10.0), 50); // z = 5（0.1 斜率 × x=50）

    // 不穿过（折线远离 mesh）→ NoIntersection 且 hits 清空。
    QVector<QPair<double, double>> far;
    far << qMakePair(500.0, -100.0) << qMakePair(500.0, 100.0);
    const SectionCut miss = intersectSurfaceWithPolyline(mesh, far);
    QCOMPARE(miss.status, SurfaceBuildStatus::NoIntersection);
    QVERIFY(miss.hits.isEmpty());

    // 退化折线拒绝。
    const SectionCut bad = intersectSurfaceWithPolyline(
        mesh, {qMakePair(1.0, 1.0)});
    QCOMPARE(bad.status, SurfaceBuildStatus::BadSection);

    // 折线多段：中间拐点也切出连续交线（两段折线拼接覆盖 mesh 跨度）。
    QVector<QPair<double, double>> bent;
    bent << qMakePair(20.0, -100.0) << qMakePair(20.0, 0.0)
         << qMakePair(80.0, 0.0) << qMakePair(80.0, 100.0);
    const SectionCut bentCut = intersectSurfaceWithPolyline(mesh, bent);
    QVERIFY2(bentCut.ok(), qPrintable(bentCut.message));
    // 深度沿线从 2（x=20）过渡到 8（x=80），单调不回跳。
    double prevZ = -1e9;
    for (const SectionHit &h : bentCut.hits)
    {
        QVERIFY(h.z >= prevZ - 1e-9);
        prevZ = h.z;
    }
}

QTEST_MAIN(TestFaultSurface)
#include "tst_faultsurface.moc"
