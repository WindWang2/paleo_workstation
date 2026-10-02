// 层：测试壳
// goal/fault-interpretation — FaultInterpretationController 编排测试：
// 编辑原语逐拍 undo/redo（Oracle #2）、SelectionContext 载荷与回声
// （Oracle #3）、同断层多层位切割独立存取经编排面（Oracle #4）、
// 写队列落盘+重开读回、catalog fault 角色链接、切割镜像层要素。
#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/metadata/faultsetstore.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/workflow/faultinterpretationcontroller.h"

#include <qgsapplication.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

using namespace paleo::fault;

namespace {
FaultStick inlineStick(int index)
{
    FaultStick s;
    s.section.kind = FaultSectionRef::Inline;
    s.section.index = index;
    s.section.displayName = QStringLiteral("IL %1").arg(index);
    s.points = {{0.0, 200.0}, {0.4, 400.0}, {1.0, 700.0}};
    s.interpreter = QStringLiteral("解释员A");
    return s;
}

FaultHorizonCut cutFor(const QString &horizon, const QString &wkt)
{
    FaultHorizonCut c;
    c.horizon = horizon;
    c.wkt = wkt;
    return c;
}
} // namespace

class TestFaultInterp : public QObject
{
    Q_OBJECT

public:
    TestFaultInterp();

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();

    void pickAndUndoRedoStepByStep();
    void stickPickAutoCreatesFaultAndUndoRemovesBoth();
    void renameAndVisibilityUndo();
    void cutsIndependentPerHorizonViaController();
    void selectionPayloadAndEcho();
    void persistsThroughWriteQueueAndReopens();
    void catalogFaultRoleLinkEnsured();
    void mapLayerMirrorFeatures();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    PaleoProjectStore m_projectStore;
    FaultSetStore m_store{QString(), nullptr};
    SelectionContext m_selection;
    FaultInterpretationController m_ctl;
    QString dbPath() const { return m_dir->path() + "/project.sqlite"; }
};

TestFaultInterp::TestFaultInterp()
    : m_store(QString(), nullptr), m_ctl(&m_store, &m_selection)
{
}

void TestFaultInterp::initTestCase()
{
    QgsApplication::initQgis();
}

void TestFaultInterp::cleanupTestCase()
{
    QgsApplication::exitQgis();
}

void TestFaultInterp::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    m_projectStore.setProjectPaths(m_dir->path() + "/p.qgz", m_dir->path() + "/p.gpkg",
                                   dbPath());
    m_store = FaultSetStore(dbPath(), &m_projectStore);
    m_store.open(nullptr);
    m_ctl.reload(nullptr);
    m_ctl.editStack()->clear();
    m_selection.clear(QStringLiteral("test"));
}

// Oracle #2：拾取/删除/改名逐拍 undo/redo。
void TestFaultInterp::pickAndUndoRedoStepByStep()
{
    // 建 F1，拾 2 棒，删 1 棒，改名，再全部回退再重放——每拍断言。
    const QString f1 = m_ctl.addFault(QStringLiteral("F1"));
    QCOMPARE(m_ctl.faultSet().faultCount(), 1);
    QCOMPARE(m_ctl.editStack()->count(), 1);

    const auto s1 = m_ctl.addStick(inlineStick(120));
    QCOMPARE(s1.first, f1);
    QCOMPARE(m_ctl.faultSet().faultById(f1)->sticks.size(), 1);
    QCOMPARE(m_ctl.editStack()->count(), 2);

    m_ctl.setActiveFaultId(f1);
    const auto s2 = m_ctl.addStick(inlineStick(120)); // 同剖面第二棒
    QCOMPARE(m_ctl.faultSet().faultById(f1)->sticks.size(), 2);
    QCOMPARE(m_ctl.editStack()->count(), 3);

    QVERIFY(m_ctl.removeStick(f1, s1.second));
    QCOMPARE(m_ctl.faultSet().faultById(f1)->sticks.size(), 1);
    QCOMPARE(m_ctl.editStack()->count(), 4);

    QVERIFY(m_ctl.renameFault(f1, QStringLiteral("主断层")));
    QCOMPARE(m_ctl.faultSet().faultById(f1)->name, QStringLiteral("主断层"));
    QCOMPARE(m_ctl.editStack()->count(), 5);

    // 逐拍 undo：改名 → 删棒 → 拾棒2 → 拾棒1 → 建断层
    m_ctl.editStack()->undo();
    QCOMPARE(m_ctl.faultSet().faultById(f1)->name, QStringLiteral("F1"));
    m_ctl.editStack()->undo();
    QCOMPARE(m_ctl.faultSet().faultById(f1)->sticks.size(), 2);
    m_ctl.editStack()->undo();
    QCOMPARE(m_ctl.faultSet().faultById(f1)->sticks.size(), 1);
    QCOMPARE(m_ctl.faultSet().faultById(f1)->sticks.at(0).id, s1.second);
    m_ctl.editStack()->undo();
    QCOMPARE(m_ctl.faultSet().faultById(f1)->sticks.size(), 0);
    m_ctl.editStack()->undo();
    QCOMPARE(m_ctl.faultSet().faultCount(), 0);

    // 逐拍 redo 全部回来（含被删棒的 id 原样恢复）
    for (int i = 0; i < 5; ++i)
        m_ctl.editStack()->redo();
    QCOMPARE(m_ctl.faultSet().faultCount(), 1);
    QCOMPARE(m_ctl.faultSet().faultById(f1)->name, QStringLiteral("主断层"));
    QCOMPARE(m_ctl.faultSet().faultById(f1)->sticks.size(), 1);
    QCOMPARE(m_ctl.faultSet().faultById(f1)->sticks.at(0).id, s2.second);
}

void TestFaultInterp::stickPickAutoCreatesFaultAndUndoRemovesBoth()
{
    m_ctl.setActiveFaultId(QString()); // 无活动断层 → 拾取自动新建
    const auto ids = m_ctl.addStick(inlineStick(130));
    QVERIFY(!ids.first.isEmpty());
    QCOMPARE(m_ctl.faultSet().faultCount(), 1);
    QCOMPARE(m_ctl.faultSet().faultById(ids.first)->name, QStringLiteral("F")); // 基名空闲直接用
    QCOMPARE(m_ctl.activeFaultId(), ids.first); // 自动成为拾取目标

    m_ctl.editStack()->undo(); // 撤销拾取 → 掏空的自动断层一并移除
    QCOMPARE(m_ctl.faultSet().faultCount(), 0);
    m_ctl.editStack()->redo(); // 重放 → 断层与棒一起回来
    QCOMPARE(m_ctl.faultSet().faultCount(), 1);
    QCOMPARE(m_ctl.faultSet().faultById(ids.first)->sticks.size(), 1);

    // 再拾一棒后撤销首棒：断层有内容，不随首棒撤销消失
    const auto second = m_ctl.addStick(inlineStick(131));
    QCOMPARE(second.first, ids.first); // 落同一断层
    m_ctl.editStack()->undo();
    m_ctl.editStack()->undo(); // 撤销第二棒与第一棒
    QCOMPARE(m_ctl.faultSet().faultCount(), 0);
}

void TestFaultInterp::renameAndVisibilityUndo()
{
    const QString f1 = m_ctl.addFault(QStringLiteral("断层甲"));
    QVERIFY(m_ctl.setFaultVisible(f1, false));
    QVERIFY(!m_ctl.faultSet().faultById(f1)->visible);
    m_ctl.editStack()->undo();
    QVERIFY(m_ctl.faultSet().faultById(f1)->visible);
    m_ctl.editStack()->redo();
    QVERIFY(!m_ctl.faultSet().faultById(f1)->visible);

    // 改名撞名拒绝（不产生命令）
    m_ctl.addFault(QStringLiteral("断层乙"));
    const int countBefore = m_ctl.editStack()->count();
    QVERIFY(!m_ctl.renameFault(QStringLiteral("f-1"), QStringLiteral("断层乙")));
    QCOMPARE(m_ctl.editStack()->count(), countBefore);
}

// Oracle #4：同一断层对多层位的切割经编排面独立存取。
void TestFaultInterp::cutsIndependentPerHorizonViaController()
{
    const QString f1 = m_ctl.addFault(QStringLiteral("F1"));
    const QString wktA = QStringLiteral("Polygon ((0 0, 4 0, 4 4, 0 4, 0 0))");
    const QString wktB = QStringLiteral("Polygon ((10 10, 14 10, 14 14, 10 14, 10 10))");

    QVERIFY(m_ctl.setCut(f1, cutFor(QStringLiteral("H1"), wktA)));
    QVERIFY(m_ctl.setCut(f1, cutFor(QStringLiteral("H2"), wktB)));
    QCOMPARE(m_ctl.faultSet().faultById(f1)->cuts.size(), 2);

    // 上盘方向各层位独立设置（H2 左、H1 右），互不影响
    QVERIFY(m_ctl.setCutHangingSide(f1, "H2", FaultHangingSide::Left));
    QVERIFY(m_ctl.setCutHangingSide(f1, "H1", FaultHangingSide::Right));
    QCOMPARE(m_ctl.faultSet().cut(f1, "H1")->hangingSide, FaultHangingSide::Right);
    QCOMPARE(m_ctl.faultSet().cut(f1, "H2")->hangingSide, FaultHangingSide::Left);

    // H1 重画替换（同层位），H2 原样
    QVERIFY(m_ctl.setCut(f1, cutFor(QStringLiteral("H1"),
                                    QStringLiteral("Polygon ((20 20, 24 20, 24 24, 20 24, 20 20))"))));
    QCOMPARE(m_ctl.faultSet().faultById(f1)->cuts.size(), 2);
    QCOMPARE(m_ctl.faultSet().cut(f1, "H1")->wkt.startsWith(QStringLiteral("Polygon ((20")),
             true);
    QCOMPARE(m_ctl.faultSet().cut(f1, "H2")->wkt, wktB);

    // undo 链逐拍回退切割面：H1 替换 → H1 方向 → H2 方向 → H2 画 → H1 画
    m_ctl.editStack()->undo(); // H1 替换回退
    QCOMPARE(m_ctl.faultSet().cut(f1, "H1")->wkt, wktA);
    QCOMPARE(m_ctl.faultSet().cut(f1, "H1")->hangingSide, FaultHangingSide::Right);
    m_ctl.editStack()->undo(); // H1 方向回退
    QCOMPARE(m_ctl.faultSet().cut(f1, "H1")->hangingSide, FaultHangingSide::Unknown);
    m_ctl.editStack()->undo(); // H2 方向回退
    QCOMPARE(m_ctl.faultSet().cut(f1, "H2")->hangingSide, FaultHangingSide::Unknown);
    m_ctl.editStack()->undo(); // H2 画回退
    QVERIFY(m_ctl.faultSet().cut(f1, "H2") == nullptr);
    QVERIFY(m_ctl.faultSet().cut(f1, "H1") != nullptr);
}

// Oracle #3：选中断层经 SelectionContext 广播——断言信号载荷，不截屏。
void TestFaultInterp::selectionPayloadAndEcho()
{
    const QString f1 = m_ctl.addFault(QStringLiteral("F1"));
    m_ctl.addFault(QStringLiteral("F2"));
    QSignalSpy payloadSpy(&m_selection, &SelectionContext::selectionChanged);
    QSignalSpy echoSpy(&m_ctl, &FaultInterpretationController::faultSelectionChanged);

    m_ctl.selectFaults({f1, QStringLiteral("f-404")}, QStringLiteral("fault_panel"));
    QCOMPARE(payloadSpy.size(), 1);
    const auto payload = payloadSpy.takeFirst();
    QCOMPARE(payload.at(0).toStringList(), QStringList{QStringLiteral("fault:") + f1});
    QCOMPARE(payload.at(1).toString(), QStringLiteral("fault_panel"));
    QCOMPARE(echoSpy.size(), 1); // 回声（前缀已剥）
    QCOMPARE(echoSpy.takeFirst().at(0).toStringList(), QStringList{f1});
    QCOMPARE(m_ctl.selectedFaultIds(), QStringList{f1});

    // 空选择广播清空；不存在的 id 不进载荷
    m_ctl.selectFaults({}, QStringLiteral("fault_panel"));
    QCOMPARE(m_ctl.selectedFaultIds(), QStringList());
    QCOMPARE(m_ctl.faultIdFromSelectionId(QStringLiteral("fault:f-9")),
             QStringLiteral("f-9"));
    QVERIFY(m_ctl.faultIdFromSelectionId(QStringLiteral("well:w-1")).isEmpty());
}

// Oracle #1 存储面：编排原语自动落盘，重开读回完整。
void TestFaultInterp::persistsThroughWriteQueueAndReopens()
{
    const QString f1 = m_ctl.addFault(QStringLiteral("F1"), QStringLiteral("解释员A"));
    m_ctl.addStick(inlineStick(120));
    m_ctl.setCut(f1, cutFor(QStringLiteral("H1"),
                            QStringLiteral("Polygon ((0 0, 1 0, 1 1, 0 1, 0 0))")));
    QVERIFY(m_ctl.lastError().isEmpty());

    // 重开（新 store + 新 controller，同库）
    FaultSetStore reopened(dbPath());
    reopened.open(nullptr);
    FaultInterpretationController ctl2(&reopened, nullptr);
    QVERIFY(ctl2.reload(nullptr));
    QCOMPARE(ctl2.faultSet().faultCount(), 1);
    const Fault *f = ctl2.faultSet().faultById(f1);
    QVERIFY(f != nullptr);
    QCOMPARE(f->interpreter, QStringLiteral("解释员A"));
    QCOMPARE(f->sticks.size(), 1);
    QCOMPARE(f->sticks.at(0).section.index, 120);
    QCOMPARE(f->cuts.size(), 1);
    QCOMPARE(f->cuts.at(0).horizon, QStringLiteral("H1"));
}

void TestFaultInterp::catalogFaultRoleLinkEnsured()
{
    // 最小 catalog：一 survey 实体 + 一地震体资产 + 主链接
    DataCatalog catalog;
    QString catErr;
    QVERIFY2(catalog.open(m_dir->path(), &catErr), qPrintable(catErr));
    CatalogEntity entity;
    entity.id = QStringLiteral("svy-1");
    entity.entityType = QStringLiteral("seismic_survey");
    entity.name = QStringLiteral("三维工区");
    QVERIFY(catalog.addEntity(entity, nullptr));
    CatalogAsset asset;
    asset.id = QStringLiteral("ast-1");
    asset.type = QStringLiteral("seismic");
    asset.format = QStringLiteral("sgy");
    asset.displayName = QStringLiteral("volume.sgy");
    QVERIFY(catalog.addAsset(asset, nullptr));
    EntityAssetLink link;
    link.entityType = QStringLiteral("seismic_survey");
    link.entityId = entity.id;
    link.assetId = asset.id;
    link.role = QStringLiteral("seismic_volume");
    QVERIFY(catalog.addLink(link, nullptr));

    // 空集不挂链
    m_ctl.setCatalogContext(&catalog, entity.id, asset.id);
    int faultLinks = 0;
    for (const EntityAssetLink &l : catalog.linksForAsset(asset.id))
        if (l.role == QLatin1String("fault"))
            ++faultLinks;
    QCOMPARE(faultLinks, 0);

    // 首个内容落地后 ensure 一条 fault 角色链接（词表既有角色）
    const QString f1 = m_ctl.addFault(QStringLiteral("F1"));
    m_ctl.addStick(inlineStick(120));
    faultLinks = 0;
    // linksForAsset 按值返回：先落到局部，faultLink 才不会指向已销毁的临时容器。
    const auto links = catalog.linksForAsset(asset.id);
    const EntityAssetLink *faultLink = nullptr;
    for (const EntityAssetLink &l : links) {
        if (l.role == QLatin1String("fault")) {
            ++faultLinks;
            faultLink = &l;
        }
    }
    QCOMPARE(faultLinks, 1);
    QVERIFY(faultLink != nullptr);
    QCOMPARE(faultLink->entityId, entity.id);
    QCOMPARE(faultLink->entityType, QStringLiteral("seismic_survey"));
    Q_UNUSED(f1);
    // 幂等：再改一次不重复挂
    m_ctl.renameFault(f1, QStringLiteral("F2"));
    faultLinks = 0;
    for (const EntityAssetLink &l : catalog.linksForAsset(asset.id))
        if (l.role == QLatin1String("fault"))
            ++faultLinks;
    QCOMPARE(faultLinks, 1);
}

void TestFaultInterp::mapLayerMirrorFeatures()
{
    const QString f1 = m_ctl.addFault(QStringLiteral("F1"));
    m_ctl.addStick(inlineStick(120));
    QVERIFY(m_ctl.setCut(f1, cutFor(QStringLiteral("H1"),
                                    QStringLiteral("Polygon ((0 0, 1 0, 1 1, 0 1, 0 0))"))));
    m_ctl.setCut(f1, cutFor(QStringLiteral("H2"),
                            QStringLiteral("Polygon ((2 2, 3 2, 3 3, 2 3, 2 2))")));

    QgsVectorLayer *layer = m_ctl.ensureMapLayer(QStringLiteral("EPSG:3857"));
    QVERIFY(layer != nullptr && layer->isValid());
    QCOMPARE(int(layer->featureCount()), 2); // 两层位切割各一要素（棒不上图）

    // 隐藏断层 → 镜像层整刷后无要素
    m_ctl.setFaultVisible(f1, false);
    QCOMPARE(int(layer->featureCount()), 0);
    m_ctl.setFaultVisible(f1, true);
    QCOMPARE(int(layer->featureCount()), 2);

    // 地图选中要素 → SelectionContext 广播（origin=fault_map）
    QSignalSpy payloadSpy(&m_selection, &SelectionContext::selectionChanged);
    QgsFeature first;
    layer->getFeatures().nextFeature(first);
    layer->select(first.id());
    QCOMPARE(payloadSpy.size(), 1);
    const QVariantList row = payloadSpy.takeFirst();
    QCOMPARE(row.at(0).toStringList(), QStringList{QStringLiteral("fault:") + f1});
    QCOMPARE(row.at(1).toString(), QStringLiteral("fault_map"));

    // 反向：面板选择 → 地图要素被选中（回声）
    m_ctl.selectFaults({f1}, QStringLiteral("fault_panel"));
    QCOMPARE(layer->selectedFeatureCount(), 2); // 该断层全部切割要素
    QgsProject::instance()->removeMapLayer(layer);
}

QTEST_MAIN(TestFaultInterp)
#include "tst_faultinterp.moc"
