// 层：测试壳
// goal/fault-surface — 断棒成面、断距、剖面交线、体域阻断、性能。
#include <QtTest>
#include <QElapsedTimer>
#include <QSet>
#include <QtMath>

#include <cmath>

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
    void hundredByTwoHundredUnderThreeSeconds();
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

QTEST_MAIN(TestFaultSurface)
#include "tst_faultsurface.moc"
