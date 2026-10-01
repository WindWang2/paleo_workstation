// 层：测试壳
// goal/fault-interpretation — 剖面断层拾取 UI 测试：
// IL 剖面拾取 → FaultSet 落账 → 画布回显（Oracle #1/剖面腿）、时间切片
// 拒拾、任意线剖面身份稳定（同路径重提取棒回显）、联动高亮（Oracle #3）、
// 面板树/切割绘制意图、保存重开完整还原（Oracle #1 全闭环 offscreen）。
#include <QtTest>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <cmath>
#include <cstring>
#include <memory>

#include "../src/linkage/selectioncontext.h"
#include "../src/metadata/faultsetstore.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/seismictaskservice.h"
#include "../src/ui/faults/faultmanagerpanel.h"
#include "../src/ui/seismicsection/seismicsectiondockwidget.h"
#include "../src/workflow/faultinterpretationcontroller.h"

#include <qgsapplication.h>

using namespace paleo::fault;

namespace {
seismic::SgySliceImage makeSlice(int cols, int rows)
{
    seismic::SgySliceImage img;
    img.width = cols;
    img.height = rows;
    img.valueMin = -1.0f;
    img.valueMax = 1.0f;
    img.values.assign(std::size_t(cols) * rows, 0.1f);
    return img;
}

seismic::SectionRef inlineRef(int inlineNo, int colMin, int colMax)
{
    seismic::SectionRef ref;
    ref.valid = true;
    ref.type = seismic::SgySliceType::Inline;
    ref.index = inlineNo;
    ref.colMin = colMin;
    ref.colMax = colMax;
    return ref;
}

// 与 tst_seismic_interpret 同款合成 SEG-Y（大端样点 + 完整道头）
bool writeTestSegy(const QString &filePath, int inlines, int xlines, int ns)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QByteArray(3200, ' '));
    QByteArray binHdr(400, 0);
    const auto put16 = [&](QByteArray &buf, int at, qint16 v) {
        buf[at] = char(quint8(v >> 8));
        buf[at + 1] = char(quint8(v));
    };
    const auto put32 = [&](QByteArray &buf, int at, qint32 v) {
        buf[at] = char(quint8(v >> 24));
        buf[at + 1] = char(quint8(v >> 16));
        buf[at + 2] = char(quint8(v >> 8));
        buf[at + 3] = char(quint8(v));
    };
    put16(binHdr, 12, qint16(xlines));
    put16(binHdr, 16, 2000);
    put16(binHdr, 20, qint16(ns));
    put16(binHdr, 24, 5);
    file.write(binHdr);
    for (int i = 0; i < inlines; ++i)
        for (int j = 0; j < xlines; ++j) {
            QByteArray trHdr(240, 0);
            put32(trHdr, 0, i * xlines + j + 1);
            put32(trHdr, 188, 1000 + i);
            put32(trHdr, 192, 2000 + j);
            put16(trHdr, 114, qint16(ns));
            file.write(trHdr);
            QByteArray samples(ns * 4, 0);
            for (int k = 0; k < ns; ++k) {
                const float val = float((i + 1) * 100 + j) + k * 0.25f;
                quint32 bits;
                std::memcpy(&bits, &val, 4);
                bits = qToBigEndian(bits);
                std::memcpy(samples.data() + k * 4, &bits, 4);
            }
            file.write(samples);
        }
    file.close();
    return QFileInfo(filePath).size() > 3600;
}

const QVector<QPair<double, double>> kStickPoints = {
    {0.0, 120.0}, {0.5, 360.0}, {1.0, 600.0}};
} // namespace

class TestFaultSectionUi : public QObject
{
    Q_OBJECT

public:
    TestFaultSectionUi();
    ~TestFaultSectionUi() override;

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();

    void ilPickLandsInFaultSetAndRedraws();
    void timeSlicePickRejected();
    void pickUndoRemovesStickAndOverlay();
    void arbitrarySectionIdentityIsStable();
    void selectionHighlightOnCanvas();
    void panelTreeAndCutIntent();
    void fullLoopSaveAndReopenRestores();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    PaleoProjectStore m_projectStore;
    FaultSetStore m_store{QString(), nullptr};
    SelectionContext m_selection;
    FaultInterpretationController m_ctl;
    QString dbPath() const { return m_dir->path() + "/project.sqlite"; }

    // IL 120 剖面 dock（画布已喂合成数据 + 有效 SectionRef）
    seismic::SeismicSectionDockWidget *makeIlDock();
};

TestFaultSectionUi::TestFaultSectionUi()
    : m_ctl(&m_store, &m_selection)
{
}

TestFaultSectionUi::~TestFaultSectionUi() = default;

void TestFaultSectionUi::initTestCase()
{
    QgsApplication::initQgis();
}

void TestFaultSectionUi::cleanupTestCase()
{
    QgsApplication::exitQgis();
}

void TestFaultSectionUi::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    m_projectStore.setProjectPaths(m_dir->path() + "/p.qgz", m_dir->path() + "/p.gpkg",
                                   dbPath());
    m_store = FaultSetStore(dbPath(), &m_projectStore);
    m_store.open(nullptr);
    m_ctl.reload(nullptr);
    m_ctl.editStack()->clear();
    m_ctl.setActiveFaultId(QString());
    m_selection.clear(QStringLiteral("test"));
}

seismic::SeismicSectionDockWidget *TestFaultSectionUi::makeIlDock()
{
    auto *dock = new seismic::SeismicSectionDockWidget;
    dock->canvas()->setSectionData(makeSlice(8, 64));
    dock->canvas()->setSectionRef(inlineRef(120, 2000, 2007));
    dock->setFaultController(&m_ctl);
    return dock;
}

void TestFaultSectionUi::ilPickLandsInFaultSetAndRedraws()
{
    auto *dock = makeIlDock();
    QSignalSpy changedSpy(&m_ctl, &FaultInterpretationController::faultSetChanged);
    dock->addFaultFromCanvas(kStickPoints);
    QCOMPARE(changedSpy.size(), 1);
    QCOMPARE(m_ctl.faultSet().faultCount(), 1); // 自动新建断层
    const Fault *f = m_ctl.faultSet().faults().at(0).sticks.isEmpty()
        ? nullptr : &m_ctl.faultSet().faults().at(0);
    QVERIFY(f != nullptr);
    QCOMPARE(f->sticks.size(), 1);
    QCOMPARE(f->sticks.at(0).section.kind, FaultSectionRef::Inline);
    QCOMPARE(f->sticks.at(0).section.index, 120);
    QCOMPARE(f->sticks.at(0).points, kStickPoints);
    // 画布回显：faultSetChanged → dock 过滤当前剖面 → 画布叠加 1 根
    QCOMPARE(dock->canvas()->faultStickOverlays().size(), 1);
    QCOMPARE(dock->canvas()->faultStickOverlays().at(0).points, kStickPoints);
    // legacy 会话不双写（FaultSet 是权威路径）
    QCOMPARE(dock->interpretationSession().faults.size(), 0);
    delete dock;
}

void TestFaultSectionUi::timeSlicePickRejected()
{
    auto *dock = new seismic::SeismicSectionDockWidget;
    dock->canvas()->setSectionData(makeSlice(8, 64));
    seismic::SectionRef ref;
    ref.valid = true;
    ref.type = seismic::SgySliceType::Time;
    ref.index = 500;
    dock->canvas()->setSectionRef(ref);
    dock->setFaultController(&m_ctl);
    dock->addFaultFromCanvas(kStickPoints);
    QCOMPARE(m_ctl.faultSet().faultCount(), 0); // 时间切片无剖面身份 → 拒
    QCOMPARE(dock->canvas()->faultStickOverlays().size(), 0);
    delete dock;
}

void TestFaultSectionUi::pickUndoRemovesStickAndOverlay()
{
    auto *dock = makeIlDock();
    dock->addFaultFromCanvas(kStickPoints);
    QCOMPARE(dock->canvas()->faultStickOverlays().size(), 1);
    m_ctl.editStack()->undo(); // 剖面拾取经编排器栈撤销（Oracle #2 剖面腿）
    QCOMPARE(m_ctl.faultSet().faultCount(), 0);
    QCOMPARE(dock->canvas()->faultStickOverlays().size(), 0);
    m_ctl.editStack()->redo();
    QCOMPARE(m_ctl.faultSet().faultCount(), 1);
    QCOMPARE(dock->canvas()->faultStickOverlays().size(), 1);
    delete dock;
}

void TestFaultSectionUi::arbitrarySectionIdentityIsStable()
{
    const QString sgy = m_dir->filePath("arb.sgy");
    QVERIFY2(writeTestSegy(sgy, 6, 6, 64), "写测试 SEG-Y");
    auto volume = std::make_shared<seismic::SgyVolume>();
    std::string loadErr;
    QVERIFY2(volume->Load(sgy.toStdString(), loadErr), loadErr.c_str());

    PaleoTaskService tasks;
    seismic::SeismicTaskService svc(&tasks);
    auto *dock = new seismic::SeismicSectionDockWidget;
    dock->setTaskService(&svc);
    dock->setVolume(volume);
    dock->setFaultController(&m_ctl);

    const std::vector<glm::ivec2> pathA = {{1000, 2000}, {1002, 2003}};
    QSignalSpy doneSpy(dock, &seismic::SeismicSectionDockWidget::sectionExtractionFinished);
    dock->extractSectionFromVolumeAsync(volume, pathA, QStringLiteral("任意线A"));
    QVERIFY2(doneSpy.wait(30000), "任意线提取超时");
    QVERIFY(doneSpy.takeFirst().at(0).toBool());

    paleo::fault::FaultSectionRef section;
    QVERIFY2(dock->currentFaultSection(&section), "任意线应有剖面身份");
    QCOMPARE(section.kind, FaultSectionRef::Arbitrary);
    QCOMPARE(section.pathId, QStringLiteral("1000,2000;1002,2003"));

    dock->addFaultFromCanvas(kStickPoints);
    QCOMPARE(m_ctl.faultSet().faultCount(), 1);
    QCOMPARE(dock->canvas()->faultStickOverlays().size(), 1); // 任意线上回显

    // 同路径重提取 → 身份复现 → 已有棒继续回显（重开等价语义）
    QSignalSpy againSpy(dock, &seismic::SeismicSectionDockWidget::sectionExtractionFinished);
    dock->extractSectionFromVolumeAsync(volume, pathA, QStringLiteral("任意线A"));
    QVERIFY2(againSpy.wait(30000), "任意线二次提取超时");
    againSpy.wait(100);
    QCOMPARE(dock->canvas()->faultStickOverlays().size(), 1);

    // 换路径 → 身份不同 → 不回显
    const std::vector<glm::ivec2> pathB = {{1001, 2000}, {1001, 2004}};
    QSignalSpy otherSpy(dock, &seismic::SeismicSectionDockWidget::sectionExtractionFinished);
    dock->extractSectionFromVolumeAsync(volume, pathB, QStringLiteral("任意线B"));
    QVERIFY2(otherSpy.wait(30000), "任意线B提取超时");
    otherSpy.wait(100);
    paleo::fault::FaultSectionRef other;
    QVERIFY(dock->currentFaultSection(&other));
    QCOMPARE(other.pathId, QStringLiteral("1001,2000;1001,2004"));
    QCOMPARE(m_ctl.faultSet().sticksForSection(other).size(), 0);
    QCOMPARE(dock->canvas()->faultStickOverlays().size(), 0);

    delete dock;
}

void TestFaultSectionUi::selectionHighlightOnCanvas()
{
    auto *dock = makeIlDock();
    dock->addFaultFromCanvas(kStickPoints);
    const QString faultId = m_ctl.faultSet().faults().at(0).id;

    // 面板/地图来源的选择 → 画布高亮（白 halo 位由 canvas 断言）
    m_ctl.selectFaults({faultId}, QStringLiteral("fault_panel"));
    QCOMPARE(dock->canvas()->faultStickOverlays().size(), 1);
    QVERIFY(dock->canvas()->faultStickOverlays().at(0).highlighted);

    m_selection.clear(QStringLiteral("fault_panel"));
    QVERIFY(!dock->canvas()->faultStickOverlays().at(0).highlighted);
    delete dock;
}

void TestFaultSectionUi::panelTreeAndCutIntent()
{
    const QString f1 = m_ctl.addFault(QStringLiteral("F1"));
    m_ctl.addStick([] {
        FaultStick s;
        s.section.kind = FaultSectionRef::Inline;
        s.section.index = 120;
        s.section.displayName = "IL 120";
        s.points = kStickPoints;
        return s;
    }());

    auto *panel = new paleo::fault::FaultManagerPanel(&m_ctl, nullptr);
    QCOMPARE(panel->selectedFaultId(), QString());

    // 树上选中 F1 → 活动断层 + SelectionContext 联动（origin=fault_panel）
    QSignalSpy payloadSpy(&m_selection, &SelectionContext::selectionChanged);
    auto *item = panel->findChild<QTreeWidget *>();
    QVERIFY(item != nullptr);
    item->setCurrentItem(item->topLevelItem(0)); // 选中并置当前
    QCOMPARE(panel->selectedFaultId(), f1);
    QCOMPARE(m_ctl.activeFaultId(), f1);
    QCOMPARE(payloadSpy.size(), 1);
    QCOMPARE(payloadSpy.takeFirst().at(0).toStringList(),
             QStringList{QStringLiteral("fault:") + f1});

    // 切割绘制意图（模拟 PaleoDrawPolygonTool 收笔信号 → onCutDrawn）
    const QString wkt = QStringLiteral("Polygon ((0 0, 1 0, 1 1, 0 1, 0 0))");
    QVERIFY(QMetaObject::invokeMethod(panel, "onCutDrawn", Q_ARG(QString, wkt)));
    const FaultHorizonCut *cut = m_ctl.faultSet().cut(f1, QStringLiteral("H1"));
    QVERIFY(cut != nullptr); // horizon 空串 → 控制器取活动层位（默认 H1）
    QCOMPARE(cut->wkt, wkt);

    // 上盘方向经下拉（数据模型侧）
    QVERIFY(m_ctl.setCutHangingSide(f1, QStringLiteral("H1"), FaultHangingSide::Left));
    QCOMPARE(m_ctl.faultSet().cut(f1, "H1")->hangingSide, FaultHangingSide::Left);
    delete panel;
}

void TestFaultSectionUi::fullLoopSaveAndReopenRestores()
{
    // Oracle #1 全闭环：剖面拾取 → FaultSet 入库 → 层位图切割 → 面板管理
    // （改名）→ 保存（自动落盘）→ 重开完整还原 → 新 dock 回显断层棒。
    auto *dock = makeIlDock();
    dock->addFaultFromCanvas(kStickPoints);
    const QString faultId = m_ctl.faultSet().faults().at(0).id;

    FaultHorizonCut cut;
    cut.horizon = QStringLiteral("H1");
    cut.wkt = QStringLiteral("Polygon ((0 0, 2 0, 2 2, 0 2, 0 0))");
    cut.hangingSide = FaultHangingSide::Right;
    QVERIFY(m_ctl.setCut(faultId, cut));
    QVERIFY(m_ctl.renameFault(faultId, QStringLiteral("主断层")));
    QVERIFY(m_ctl.lastError().isEmpty()); // 全程经写队列落盘
    QCOMPARE(m_ctl.faultSet().faultCount(), 1);
    delete dock; // 关视图

    // ---- 模拟重开工程：全新 store/controller/dock，同库 ----
    FaultSetStore reopenedStore(dbPath());
    reopenedStore.open(nullptr);
    FaultInterpretationController ctl2(&reopenedStore, &m_selection);
    QVERIFY2(ctl2.reload(nullptr), "重开读回");
    QCOMPARE(ctl2.faultSet().faultCount(), 1);
    const Fault *f = ctl2.faultSet().faultById(faultId);
    QVERIFY(f != nullptr);
    QCOMPARE(f->name, QStringLiteral("主断层")); // 面板改名持久
    QCOMPARE(f->sticks.size(), 1);
    QCOMPARE(f->sticks.at(0).points, kStickPoints);
    QCOMPARE(f->sticks.at(0).section.index, 120);
    QCOMPARE(f->cuts.size(), 1);
    QCOMPARE(f->cuts.at(0).horizon, QStringLiteral("H1"));
    QCOMPARE(f->cuts.at(0).hangingSide, FaultHangingSide::Right);

    // 重开后的剖面视图：同 IL 120 → 断层棒自动回显
    auto *dock2 = new seismic::SeismicSectionDockWidget;
    dock2->canvas()->setSectionData(makeSlice(8, 64));
    dock2->canvas()->setSectionRef(inlineRef(120, 2000, 2007));
    dock2->setFaultController(&ctl2);
    QCOMPARE(dock2->canvas()->faultStickOverlays().size(), 1);
    QCOMPARE(dock2->canvas()->faultStickOverlays().at(0).points, kStickPoints);

    // 重开后的管理面板：树上有断层且名称正确
    auto *panel2 = new paleo::fault::FaultManagerPanel(&ctl2, nullptr);
    auto *tree2 = panel2->findChild<QTreeWidget *>();
    QVERIFY(tree2 != nullptr && tree2->topLevelItemCount() == 1);
    QCOMPARE(tree2->topLevelItem(0)->text(0), QStringLiteral("主断层"));
    delete panel2;
    delete dock2;
}

int main(int argc, char *argv[])
{
    // 与 tst_faultinterp 同款：QApplication（offscreen）+ initQgis 在
    // initTestCase/cleanupTestCase 内成对调用——QgsApplication 栈实例在
    // 本测试的服务/面板组合下退出期不稳（exitQgis 段错误），勿改回。
    QApplication app(argc, argv);
    TestFaultSectionUi tc;
    const int rc = QTest::qExec(&tc, argc, argv);
    return rc;
}

#include "tst_faultsectionui.moc"
