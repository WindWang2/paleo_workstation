// 层：测试壳
// goal/fault-surface — 成面编排：失败不登记，成功的 DERIVED 父版本是源 FaultSet。
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>

#include "../src/catalog/datacatalog.h"
#include "../src/metadata/faultsetstore.h"
#include "../src/workflow/derivedassets.h"
#include "../src/workflow/faultsurfaceworkflow.h"

using namespace paleo::fault;
using namespace paleo::faultsurf;

namespace {

SurveyFrame frame()
{
    SurveyFrame f;
    f.inlineStepY = 100;
    f.xlineStepX = 100;
    f.inlineMax = 10;
    f.xlineMax = 10;
    return f;
}

FaultStick stick(int index, const QString &id)
{
    FaultStick s;
    s.id = id;
    s.section.kind = FaultSectionRef::Inline;
    s.section.index = index;
    const double slope = std::tan(qDegreesToRadians(30.0));
    s.points.append({0.0, 0.0});
    s.points.append({1.0, 1000.0 * slope});
    return s;
}

Fault twoSticks()
{
    Fault fault;
    fault.id = QStringLiteral("f-1");
    fault.name = QStringLiteral("F1");
    fault.sticks.append(stick(0, QStringLiteral("s-1")));
    fault.sticks.append(stick(2, QStringLiteral("s-2")));
    return fault;
}

} // namespace

class TestFaultSurfaceWorkflow : public QObject
{
    Q_OBJECT

private slots:
    void failedBuildDoesNotRegister();
    void produceCommitsDerivedChildOfFaultSetVersion();
    void saveToStoreRoundTripsMesh();
};

void TestFaultSurfaceWorkflow::failedBuildDoesNotRegister()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog catalog;
    QVERIFY(catalog.open(dir.path(), nullptr));
    CatalogAsset parentAsset;
    parentAsset.id = QStringLiteral("ast-fault");
    parentAsset.type = QStringLiteral("fault_set");
    parentAsset.format = QStringLiteral("json");
    parentAsset.displayName = QStringLiteral("fault-set");
    QVERIFY(catalog.addAsset(parentAsset, nullptr));
    CatalogVersion parent;
    parent.id = QStringLiteral("ver-parent");
    parent.assetId = parentAsset.id;
    parent.stage = QStringLiteral("RAW");
    parent.versionNumber = 1;
    parent.fileName = QStringLiteral("fault-set.json");
    QVERIFY(catalog.addVersion(parent, nullptr));
    QCOMPARE(catalog.versions().size(), 1);

    Fault fault;
    fault.id = QStringLiteral("f-1");
    fault.name = QStringLiteral("F1");
    fault.sticks.append(stick(0, QStringLiteral("s-only")));
    DerivedAssetRegistrar registrar(&catalog, dir.path());
    FaultSurfaceRequest request;
    request.fault = fault;
    request.frame = frame();
    request.parentVersionId = parent.id;
    const FaultSurfaceProduceResult result = FaultSurfaceWorkflow().produce(request, registrar);
    QVERIFY(!result.ok);
    QCOMPARE(result.status, SurfaceBuildStatus::TooFewSections);
    QVERIFY(result.versionId.isEmpty());
    QCOMPARE(catalog.versions().size(), 1);

    request.parentVersionId.clear();
    request.fault = twoSticks();
    const FaultSurfaceProduceResult missingParent = FaultSurfaceWorkflow().produce(request, registrar);
    QVERIFY(!missingParent.ok);
    QVERIFY(missingParent.error.contains(QStringLiteral("FaultSet")));
    QCOMPARE(catalog.versions().size(), 1);
}

void TestFaultSurfaceWorkflow::produceCommitsDerivedChildOfFaultSetVersion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog catalog;
    QVERIFY(catalog.open(dir.path(), nullptr));
    CatalogAsset parentAsset;
    parentAsset.id = QStringLiteral("ast-fault");
    parentAsset.type = QStringLiteral("fault_set");
    parentAsset.format = QStringLiteral("json");
    parentAsset.displayName = QStringLiteral("fault-set");
    QVERIFY(catalog.addAsset(parentAsset, nullptr));
    CatalogVersion parent;
    parent.id = QStringLiteral("ver-parent");
    parent.assetId = parentAsset.id;
    parent.stage = QStringLiteral("RAW");
    parent.versionNumber = 1;
    parent.fileName = QStringLiteral("fault-set.json");
    QVERIFY(catalog.addVersion(parent, nullptr));

    DerivedAssetRegistrar registrar(&catalog, dir.path());
    FaultSurfaceRequest request;
    request.fault = twoSticks();
    request.frame = frame();
    request.parentVersionId = parent.id;
    const FaultSurfaceProduceResult result = FaultSurfaceWorkflow().produce(request, registrar);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(!result.versionId.isEmpty());
    CatalogVersion derived;
    bool found = false;
    for (const CatalogVersion &version : catalog.versions()) {
        if (version.id == result.versionId) {
            derived = version;
            found = true;
        }
    }
    QVERIFY(found);
    QCOMPARE(derived.stage, QStringLiteral("DERIVED"));
    QCOMPARE(derived.parentVersionIds, QStringList({parent.id}));
    QVERIFY(QFile::exists(dir.filePath(result.relativePath)));
}

void TestFaultSurfaceWorkflow::saveToStoreRoundTripsMesh()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    FaultSet set;
    const QString id = set.addFault(QStringLiteral("F1"));
    QVERIFY(set.addStick(id, stick(0, QStringLiteral("s-1"))));
    QVERIFY(set.addStick(id, stick(2, QStringLiteral("s-2"))));
    const FaultSurfaceProduceResult built = FaultSurfaceWorkflow().build(set.faults().at(0), frame());
    QVERIFY2(built.ok, qPrintable(built.error));

    FaultSetStore store(dir.filePath(QStringLiteral("project.sqlite")));
    QVERIFY(store.open(nullptr));
    QVERIFY(FaultSurfaceWorkflow().saveToStore(store, set, id, built.mesh));

    FaultSet loaded;
    QVERIFY(store.load(loaded));
    QCOMPARE(loaded.faultById(id)->surface.vertices.size(), built.mesh.vertices.size());
    QCOMPARE(loaded.faultById(id)->surface.triangles.size(), built.mesh.triangles.size());
    QCOMPARE(loaded.faultById(id)->sticks.size(), 2);
}

QTEST_MAIN(TestFaultSurfaceWorkflow)
#include "tst_faultsurfaceworkflow.moc"
