#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgslayertree.h>
#include <qgslayertreegroup.h>
#include <qgslayertreelayer.h>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsmapsettings.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

#include "../src/catalog/datacatalog.h"
#include "../src/qgis/qgiseditingservice.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/metadata/layermanifest.h"
#include "../src/qgis/qgiscanvascontroller.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"

// Fixture path: prefer the build-provided define, else derive from this file's
// location so standalone g++ builds work too.
static QString fixtureGpkg()
{
#ifdef FIXTURE_GPKG
  return QStringLiteral(FIXTURE_GPKG);
#else
  const QString testsDir = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath();
  return QDir(testsDir).absoluteFilePath(QStringLiteral("../testdata/fixture.gpkg"));
#endif
}

static LayerDeclaration decl(const QString &layerId, const QString &horizon,
                             const QString &type = QStringLiteral("vector"))
{
  LayerDeclaration d;
  d.layerId = layerId;
  d.horizon = horizon;
  d.type = type;
  d.source = fixtureGpkg() + QStringLiteral("|layername=basin");
  d.styleRef = QStringLiteral("styles/%1.qml").arg(layerId);
  d.group = QStringLiteral("04_SingleFactor");
  return d;
}

// §37 spine acceptance: manifest is the layer-SET authority; the service
// materializes only the active horizon's declared layers into QgsProject.
class TestLayerService : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY(QgsApplication::instance() != nullptr);
    QVERIFY2(QFile::exists(fixtureGpkg()),
             qPrintable(QStringLiteral("fixture missing: %1").arg(fixtureGpkg())));
  }

  void cleanup()
  {
    // Tests share the QgsProject singleton via the null-projectSvc fallback.
    QgsProject::instance()->removeAllMapLayers();
    // 组节点不随图层注册表清空——逐节点摘掉（分组归位后测试间不互染）。
    QgsLayerTree *root = QgsProject::instance()->layerTreeRoot();
    const auto kids = root->children();
    for (QgsLayerTreeNode *n : kids)
      root->removeChildNode(n);
  }

  // (a) declare 3 layers on 2 horizons → all()==3, forHorizon splits correctly
  void declaredSetSplitsByHorizon()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
    QString err;
    QVERIFY2(manifest.open(&err), qPrintable(err));

    QgisLayerService svc(nullptr, &manifest);
    QVERIFY2(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1")), &err), qPrintable(err));
    QVERIFY(svc.declare(decl(QStringLiteral("faults.T1"), QStringLiteral("T1"))));
    QVERIFY(svc.declare(decl(QStringLiteral("facies.T2"), QStringLiteral("T2"))));
    // upsert of an existing id must not duplicate the declaration
    QVERIFY(svc.declare(decl(QStringLiteral("facies.T2"), QStringLiteral("T2"))));

    QCOMPARE(manifest.all().size(), 3);
    QCOMPARE(svc.declared().size(), 3);

    const QVector<LayerDeclaration> t1 = manifest.forHorizon(QStringLiteral("T1"));
    QCOMPARE(t1.size(), 2);
    QCOMPARE(manifest.forHorizon(QStringLiteral("T2")).size(), 1);
    QCOMPARE(manifest.forHorizon(QStringLiteral("T9")).size(), 0);
    QCOMPARE(manifest.horizons(), QStringList({QStringLiteral("T1"), QStringLiteral("T2")}));

    // remove drops the declaration
    QVERIFY(manifest.remove(QStringLiteral("faults.T1")));
    QCOMPARE(manifest.all().size(), 2);
    QCOMPARE(manifest.forHorizon(QStringLiteral("T1")).size(), 1);
  }

  // (b) instantiateHorizon("T1") materializes only T1 layers
  void instantiateHorizonMaterializesOnlyTarget()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
    QVERIFY(manifest.open());
    QgisLayerService svc(nullptr, &manifest);
    QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));
    QVERIFY(svc.declare(decl(QStringLiteral("faults.T1"), QStringLiteral("T1"))));
    QVERIFY(svc.declare(decl(QStringLiteral("facies.T2"), QStringLiteral("T2"))));

    QCOMPARE(svc.instantiateHorizon(QStringLiteral("T1")), 2);

    QVERIFY(svc.isInstantiated(QStringLiteral("facies.T1")));
    QVERIFY(svc.isInstantiated(QStringLiteral("faults.T1")));
    QVERIFY(!svc.isInstantiated(QStringLiteral("facies.T2")));

    QgsMapLayer *l = svc.layer(QStringLiteral("facies.T1"));
    QVERIFY(l != nullptr);
    QVERIFY2(l->isValid(), "instantiated layer must be valid");
    QVERIFY(svc.layer(QStringLiteral("facies.T2")) == nullptr);
    QCOMPARE(QgsProject::instance()->mapLayers().size(), 2);

    // on-demand instantiation of an undeclared id fails with an error
    QString err;
    QVERIFY(svc.instantiate(QStringLiteral("ghost.layer"), &err) == nullptr);
    QVERIFY(!err.isEmpty());
    QCOMPARE(QgsProject::instance()->mapLayers().size(), 2);
  }

  // (c) setActiveHorizon T1→T2 swaps instances; T1 declarations persist
  void activeHorizonSwapsInstances()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
    QVERIFY(manifest.open());
    QgisLayerService svc(nullptr, &manifest);
    QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));
    QVERIFY(svc.declare(decl(QStringLiteral("faults.T1"), QStringLiteral("T1"))));
    QVERIFY(svc.declare(decl(QStringLiteral("facies.T2"), QStringLiteral("T2"))));

    svc.setActiveHorizon(QStringLiteral("T1"));
    QCOMPARE(svc.activeHorizon(), QStringLiteral("T1"));
    QVERIFY(svc.isInstantiated(QStringLiteral("facies.T1")));
    QVERIFY(svc.isInstantiated(QStringLiteral("faults.T1")));
    QVERIFY(!svc.isInstantiated(QStringLiteral("facies.T2")));
    QCOMPARE(QgsProject::instance()->mapLayers().size(), 2);

    svc.setActiveHorizon(QStringLiteral("T2"));
    QCOMPARE(svc.activeHorizon(), QStringLiteral("T2"));
    QVERIFY(!svc.isInstantiated(QStringLiteral("facies.T1")));
    QVERIFY(!svc.isInstantiated(QStringLiteral("faults.T1")));
    QVERIFY(svc.isInstantiated(QStringLiteral("facies.T2")));
    QVERIFY(svc.layer(QStringLiteral("facies.T1")) == nullptr);
    QCOMPARE(QgsProject::instance()->mapLayers().size(), 1);

    // declarations survive instance release — manifest is the authority
    QCOMPARE(manifest.all().size(), 3);
    QCOMPARE(manifest.forHorizon(QStringLiteral("T1")).size(), 2);

    // explicit release also drops only instances
    svc.releaseHorizon(QStringLiteral("T2"));
    QVERIFY(!svc.isInstantiated(QStringLiteral("facies.T2")));
    QCOMPARE(QgsProject::instance()->mapLayers().size(), 0);
    QCOMPARE(manifest.all().size(), 3);
  }

  // (h) releaseHorizon 遇编辑中图层：经编辑服务回滚——busy 标记随会话释放，
  //     图层仍被工程正常注销。未注入服务的裸用路径维持旧行为（rollback 照做）。
  void releaseHorizonRollsBackThroughEditingService()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    // 编辑会话会在源文件上开写句柄——用夹具副本，不动共享 testdata。
    const QString gpkgCopy = tmp.filePath(QStringLiteral("facies_copy.gpkg"));
    QVERIFY2(QFile::copy(fixtureGpkg(), gpkgCopy), qPrintable(gpkgCopy));
    LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
    QVERIFY(manifest.open());
    QgisLayerService svc(nullptr, &manifest);
    LayerDeclaration editable = decl(QStringLiteral("facies.T1"), QStringLiteral("T1"));
    editable.source = gpkgCopy + QStringLiteral("|layername=basin");
    QVERIFY(svc.declare(editable));
    QCOMPARE(svc.instantiateHorizon(QStringLiteral("T1")), 1);
    auto *vl = qobject_cast<QgsVectorLayer *>(svc.layer(QStringLiteral("facies.T1")));
    QVERIFY(vl != nullptr && vl->isValid());

    PaleoProjectStore store;
    QgisEditingService editSvc(&store);
    svc.setEditingService(&editSvc);

    QString err;
    QVERIFY2(editSvc.beginEdit(vl, &err), qPrintable(err));
    QVERIFY(store.layerBusy(QStringLiteral("facies.T1"))); // busyKey = paleoLayerId 盖章值
    QVERIFY(vl->isEditable());

    QSignalSpy rolledSpy(&editSvc, &QgisEditingService::editRolledBack);
    svc.releaseHorizon(QStringLiteral("T1"));

    QVERIFY(!store.layerBusy(QStringLiteral("facies.T1"))); // busy 随会话释放
    QCOMPARE(rolledSpy.count(), 1);
    QVERIFY(!svc.isInstantiated(QStringLiteral("facies.T1")));
    QCOMPARE(QgsProject::instance()->mapLayers().size(), 0);
    QCOMPARE(manifest.all().size(), 1); // 声明仍是权威
  }

  // (d) manifest roundtrip: a fresh LayerManifest over the same sqlite file
  // sees the previously declared set intact
  void manifestSurvivesReopen()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dbPath = tmp.filePath(QStringLiteral("project.sqlite"));

    {
      LayerManifest manifest(dbPath);
      QString err;
      QVERIFY2(manifest.open(&err), qPrintable(err));
      QVERIFY(manifest.upsert(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));
      QVERIFY(manifest.upsert(decl(QStringLiteral("faults.T1"), QStringLiteral("T1"))));
      QVERIFY(manifest.upsert(decl(QStringLiteral("facies.T2"), QStringLiteral("T2"))));
    }

    {
      LayerManifest reopened(dbPath);
      QString err;
      QVERIFY2(reopened.open(&err), qPrintable(err));
      const QVector<LayerDeclaration> all = reopened.all();
      QCOMPARE(all.size(), 3);
      QCOMPARE(reopened.forHorizon(QStringLiteral("T1")).size(), 2);
      QCOMPARE(reopened.forHorizon(QStringLiteral("T2")).size(), 1);

      // fields roundtrip exactly; instantiated stays a runtime-only false
      const LayerDeclaration &f = all.at(0); // ordered by layer_id: facies.T1
      QCOMPARE(f.layerId, QStringLiteral("facies.T1"));
      QCOMPARE(f.horizon, QStringLiteral("T1"));
      QCOMPARE(f.type, QStringLiteral("vector"));
      QCOMPARE(f.source, fixtureGpkg() + QStringLiteral("|layername=basin"));
      QCOMPARE(f.styleRef, QStringLiteral("styles/facies.T1.qml"));
      QCOMPARE(f.group, QStringLiteral("04_SingleFactor"));
      QCOMPARE(f.instantiated, false);
    }
  }

  // (e) project->clear() deletes layers out from under the service — the
  // m_instances cache must purge so re-instantiate returns a fresh live layer
  // (review: dangling m_instances crash).
  void projectClearPurgesInstances()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
    QVERIFY(manifest.open());
    QgisLayerService svc(nullptr, &manifest);
    QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));

    QgsMapLayer *first = svc.instantiate(QStringLiteral("facies.T1"));
    QVERIFY(first);
    QVERIFY(first->isValid());
    QVERIFY(svc.isInstantiated(QStringLiteral("facies.T1")));

    // clear() deletes every layer — cached pointers must die with them.
    QgsProject::instance()->clear();
    QVERIFY(!svc.isInstantiated(QStringLiteral("facies.T1")));
    QVERIFY(svc.layer(QStringLiteral("facies.T1")) == nullptr);

    // Re-instantiate must hand back a live layer, never the stale pointer.
    // (A recycled heap address is legal — what matters is validity, and that
    // the layer is actually registered in the project.)
    QString err;
    QgsMapLayer *second = svc.instantiate(QStringLiteral("facies.T1"), &err);
    QVERIFY2(second && second->isValid(), qPrintable(err));
    QVERIFY(QgsProject::instance()->mapLayers().values().contains(second));
    QCOMPARE(svc.layer(QStringLiteral("facies.T1")), second);
  }

  // (f) canvas bound to the project: instantiate() lands on
  // mapSettings().layers(), release removes it (review: canvas never bound).
  void canvasTracksInstantiatedLayers()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
    QVERIFY(manifest.open());
    QgisLayerService svc(nullptr, &manifest);
    QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));

    // Bare controller — no sibling QgisProjectService — binds the
    // QgsProject::instance() fallback, same project the service writes to.
    QgisCanvasController ctl;
    QgsMapCanvas *canvas = ctl.canvas();
    QVERIFY(canvas);

    QVERIFY(svc.instantiate(QStringLiteral("facies.T1")));
    // QgsLayerTreeMapCanvasBridge defers its layer-set sync onto the event
    // loop — wait for it rather than asserting on the pending state.
    QTRY_VERIFY(canvas->mapSettings().layers().contains(
        svc.layer(QStringLiteral("facies.T1"))));

    // Canvas destination CRS is the pinned datum-free engineering grid —
    // not the fixture layer's EPSG:4326.
    const QgsCoordinateReferenceSystem dest = canvas->mapSettings().destinationCrs();
    QVERIFY(dest.isValid());
    QVERIFY2(dest.authid().isEmpty(), qPrintable(dest.authid()));
    QVERIFY(!dest.isGeographic());

    svc.releaseHorizon(QStringLiteral("T1"));
    QTRY_VERIFY(canvas->mapSettings().layers().isEmpty());
  }

  // (g) local grid CRS is a datum-free engineering CRS (PROJECT_AREA_PLAN
  // autoplan-eng): metre axes, empty authid, non-geographic, and NO
  // QgsCoordinateTransform path to EPSG:4326.
  void localGridCrsIsDatumFreeEngineering()
  {
    const QgsCoordinateReferenceSystem local =
        QgsCoordinateReferenceSystem::fromWkt(DataCatalog::localGridCrsWkt());
    QVERIFY2(local.isValid(), "local grid WKT must parse into a valid CRS");
    QVERIFY2(local.authid().isEmpty(), qPrintable(local.authid()));
    QVERIFY(!local.isGeographic());
    QCOMPARE(local.mapUnits(), Qgis::DistanceUnit::Meters);

    QgsCoordinateTransform toWgs(local,
                                 QgsCoordinateReferenceSystem(QStringLiteral("EPSG:4326")),
                                 QgsProject::instance());
    QVERIFY2(!toWgs.isValid(),
             "engineering CRS must NOT transform to EPSG:4326");
  }

  // (h) manifest read failure must surface as a READ failure, not the
  // misleading "no layer declaration" (review: error text misattribution).
  void instantiateReportsManifestReadFailure()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    // A manifest path rooted under a regular FILE can never open its sqlite.
    const QString blocker = tmp.filePath(QStringLiteral("blocker"));
    {
      QFile f(blocker);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("x");
    }
    LayerManifest manifest(blocker + QStringLiteral("/project.sqlite"));
    QgisLayerService svc(nullptr, &manifest);

    QString err;
    QVERIFY(svc.instantiate(QStringLiteral("facies.T1"), &err) == nullptr);
    QVERIFY2(!err.isEmpty(), "a manifest read failure must produce an error");
    QVERIFY2(!err.contains(QStringLiteral("no layer declaration")),
             qPrintable(err));
  }

  // (i) P1-07 / MEM-04: destroyed layer does not crash isEditingAnyLayer()
  void destroyedLayerDoesNotCrashIsEditingAnyLayer()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString copyGpkg = tmp.filePath(QStringLiteral("fixture.gpkg"));
    QVERIFY(QFile::copy(fixtureGpkg(), copyGpkg));

    LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
    QString err;
    QVERIFY2(manifest.open(&err), qPrintable(err));

    LayerDeclaration d = decl(QStringLiteral("facies.T1"), QStringLiteral("T1"));
    d.source = copyGpkg + QStringLiteral("|layername=basin");

    QgisLayerService svc(nullptr, &manifest);
    QVERIFY2(svc.declare(d, &err), qPrintable(err));

    QgsMapLayer *ml = svc.instantiate(QStringLiteral("facies.T1"), &err);
    QVERIFY2(ml, qPrintable(err));
    auto *vl = qobject_cast<QgsVectorLayer *>(ml);
    QVERIFY(vl);
    QVERIFY(vl->startEditing());
    QVERIFY(vl->isEditable());

    QString editingName;
    QVERIFY(svc.isEditingAnyLayer(&editingName));
    QCOMPARE(svc.layer(QStringLiteral("facies.T1")), ml);
    QVERIFY(svc.isInstantiated(QStringLiteral("facies.T1")));

    // Externally remove / destroy the layer from QgsProject
    QgsProject::instance()->removeMapLayer(ml->id());
    // In QGIS, removeMapLayer deletes the QObject.
    // QPointer automatically resets to nullptr.

    // Calling isEditingAnyLayer() must NOT dereference dangling memory or crash
    QVERIFY(!svc.isEditingAnyLayer(&editingName));
    QCOMPARE(svc.layer(QStringLiteral("facies.T1")), nullptr);
    QVERIFY(!svc.isInstantiated(QStringLiteral("facies.T1")));
  }

  // (j) 声明图层归组：地层大组 ⊃ 工作流子组——horizon 非空的声明层落在
  //     <层位>/<canonical 组> 路径下（"/" 子路径嵌套、旧组名折算）；horizon
  //     为空的层位无关层落根上工作流组；释放层位后空壳地层组摘除。
  void instantiatePlacesLayersUnderHorizonGroup()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
    QVERIFY(manifest.open());
    QgisLayerService svc(nullptr, &manifest);

    LayerDeclaration deep = decl(QStringLiteral("carto.T1"), QStringLiteral("T1"));
    deep.group = QStringLiteral("04_SingleFactor/Cartographic"); // "/" 子路径嵌套
    LayerDeclaration alias = decl(QStringLiteral("faults.T1"), QStringLiteral("T1"));
    alias.group = QStringLiteral("02_Constraints"); // 旧组名 → canonical 03_Constraints
    LayerDeclaration shared = decl(QStringLiteral("wells"), QString());
    shared.group = QStringLiteral("00_Data"); // 层位无关 → 根上工作流组
    QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));
    QVERIFY(svc.declare(deep));
    QVERIFY(svc.declare(alias));
    QVERIFY(svc.declare(shared));

    QCOMPARE(svc.instantiateHorizon(QStringLiteral("T1")), 3);
    QVERIFY(svc.instantiate(QStringLiteral("wells")));

    QgsLayerTree *root = QgsProject::instance()->layerTreeRoot();
    auto *t1 = root->findGroup(QStringLiteral("T1"));
    QVERIFY2(t1, "horizon group must exist at root");
    auto *sfGrp = t1->findGroup(QStringLiteral("04_SingleFactor"));
    QVERIFY2(sfGrp, "workflow subgroup must nest under horizon group");
    QVERIFY(sfGrp->findLayer(svc.layer(QStringLiteral("facies.T1"))->id()));
    auto *carto = sfGrp->findGroup(QStringLiteral("Cartographic"));
    QVERIFY2(carto, "subpath segment must nest a level deeper");
    QVERIFY(carto->findLayer(svc.layer(QStringLiteral("carto.T1"))->id()));
    auto *cons = t1->findGroup(QStringLiteral("03_Constraints"));
    QVERIFY2(cons, "legacy group name must canonicalize on placement");
    QVERIFY(cons->findLayer(svc.layer(QStringLiteral("faults.T1"))->id()));
    QVERIFY(!t1->findGroup(QStringLiteral("02_Constraints")));

    auto *data = root->findGroup(QStringLiteral("00_Data"));
    QVERIFY2(data, "horizon-agnostic layer lands in root workflow group");
    QVERIFY(data->findLayer(svc.layer(QStringLiteral("wells"))->id()));

    // 释放层位：T1 壳组摘除；agnostic 组与图层存活。
    svc.releaseHorizon(QStringLiteral("T1"));
    QVERIFY(!root->findGroup(QStringLiteral("T1")));
    QVERIFY(root->findGroup(QStringLiteral("00_Data")));
    QCOMPARE(QgsProject::instance()->mapLayers().size(), 1);
  }

  // (k) 地层组里混进手工层：releaseHorizon 只摘自动层——组因尚有后代
  //     图层而保留（剪枝守卫，防把用户手工层连锅端）。
  void releaseKeepsHorizonGroupHoldingManualLayers()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    LayerManifest manifest(tmp.filePath(QStringLiteral("project.sqlite")));
    QVERIFY(manifest.open());
    QgisLayerService svc(nullptr, &manifest);
    QVERIFY(svc.declare(decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))));
    QCOMPARE(svc.instantiateHorizon(QStringLiteral("T1")), 1);

    QgsLayerTree *root = QgsProject::instance()->layerTreeRoot();
    auto *t1 = root->findGroup(QStringLiteral("T1"));
    QVERIFY(t1);
    // 用户手工层拖进地层组（无 paleoLayerId —— release 的精确 horizon
    // 匹配不碰它，但它在树结构上挡剪枝）。
    auto *manual = new QgsVectorLayer(QStringLiteral("Point"),
                                      QStringLiteral("手工层"), QStringLiteral("memory"));
    QgsProject::instance()->addMapLayer(manual, false);
    t1->insertLayer(-1, manual);

    svc.releaseHorizon(QStringLiteral("T1"));
    QVERIFY(root->findGroup(QStringLiteral("T1")));
    QCOMPARE(QgsProject::instance()->mapLayers().size(), 1);
  }
};

int main(int argc, char *argv[])
{
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true); // distro install
  app.initQgis();
  TestLayerService tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_layerservice.moc"
