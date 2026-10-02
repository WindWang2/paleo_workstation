// 层：测试壳
// goal/fault-interpretation — FaultSetStore 持久化测试：
// project.sqlite 建表幂等、空工程读回空集、写队列注入往返（Oracle #1 存储面）、
// 无注入直写（测试路径）、只读拒绝。
#include <QtTest>
#include <QTemporaryDir>

#include <memory>

#include "../src/metadata/faultsetstore.h"
#include "../src/metadata/paleoprojectstore.h"

using namespace paleo::fault;

class TestFaultSetStore : public QObject
{
    Q_OBJECT

private slots:
    void init();

    void directRoundTrip();
    void emptyProjectLoadsEmptySet();
    void writeQueueEnforcedRoundTrip();
    void readOnlyRejectsSave();
    void reopenSeesLatestState();

private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString dbPath() const
    {
        return m_dir->path() + QStringLiteral("/project.sqlite");
    }
};

void TestFaultSetStore::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
}

void TestFaultSetStore::directRoundTrip()
{
    FaultSetStore store(dbPath());
    QString err;
    QVERIFY2(store.open(&err), qPrintable(err));

    FaultSet set;
    const QString f1 = set.addFault(QStringLiteral("F1"));
    FaultStick stick;
    stick.section.kind = FaultSectionRef::Inline;
    stick.section.index = 120;
    stick.points = {{0.0, 120.0}, {1.0, 480.0}};
    set.addStick(f1, stick);
    FaultHorizonCut cut;
    cut.horizon = QStringLiteral("H1");
    cut.wkt = QStringLiteral("Polygon ((0 0, 1 0, 1 1, 0 1, 0 0))");
    cut.hangingSide = FaultHangingSide::Right;
    set.setCut(f1, cut);
    QVERIFY2(store.save(set, &err), qPrintable(err));

    // 另一实例读回（模拟重开工程）
    FaultSetStore reloaded(dbPath());
    QVERIFY2(reloaded.open(&err), qPrintable(err));
    FaultSet back;
    QVERIFY2(reloaded.load(back, &err), qPrintable(err));
    QCOMPARE(back.faultCount(), 1);
    const Fault *f = back.faultById(f1);
    QVERIFY(f != nullptr);
    QCOMPARE(f->name, QStringLiteral("F1"));
    QCOMPARE(f->sticks.size(), 1);
    QCOMPARE(f->sticks.at(0).points.size(), 2);
    QCOMPARE(f->cuts.size(), 1);
    QCOMPARE(f->cuts.at(0).hangingSide, FaultHangingSide::Right);
}

void TestFaultSetStore::emptyProjectLoadsEmptySet()
{
    FaultSetStore store(dbPath());
    QString err;
    QVERIFY2(store.open(&err), qPrintable(err));
    FaultSet set;
    QVERIFY2(store.load(set, &err), qPrintable(err));
    QCOMPARE(set.faultCount(), 0);
}

void TestFaultSetStore::writeQueueEnforcedRoundTrip()
{
    PaleoProjectStore projectStore;
    projectStore.setProjectPaths(m_dir->path() + "/p.qgz",
                                 m_dir->path() + "/p.gpkg", dbPath());
    FaultSetStore store(dbPath(), &projectStore);
    QString err;
    QVERIFY2(store.open(&err), qPrintable(err));

    FaultSet set;
    const QString f1 = set.addFault(QStringLiteral("F1"));
    FaultStick stick;
    stick.section.kind = FaultSectionRef::Arbitrary;
    stick.section.pathId = QStringLiteral("100,200;101,201");
    stick.points = {{0.1, 200.0}, {0.9, 600.0}};
    set.addStick(f1, stick);
    QVERIFY2(store.save(set, &err), qPrintable(err));
    // 写队列串行落盘后，直连（无队列）实例立即可见
    FaultSet back;
    FaultSetStore reader(dbPath());
    QVERIFY2(reader.open(&err), qPrintable(err));
    QVERIFY2(reader.load(back, &err), qPrintable(err));
    QCOMPARE(back.faultCount(), 1);
    QCOMPARE(back.faultById(f1)->sticks.size(), 1);
}

void TestFaultSetStore::readOnlyRejectsSave()
{
    FaultSetStore store(dbPath());
    QString err;
    QVERIFY2(store.open(&err), qPrintable(err));
    store.setReadOnly(true);
    FaultSet set;
    set.addFault(QStringLiteral("F1"));
    QVERIFY(!store.save(set, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains(QStringLiteral("只读")));
}

void TestFaultSetStore::reopenSeesLatestState()
{
    FaultSetStore store(dbPath());
    QVERIFY2(store.open(nullptr), "open");
    FaultSet set;
    const QString f1 = set.addFault(QStringLiteral("F1"));
    set.renameFault(f1, QStringLiteral("主断层"));
    QVERIFY(store.save(set));

    FaultSet mid;
    FaultSetStore reader(dbPath());
    reader.open(nullptr);
    reader.load(mid);
    QCOMPARE(mid.faultById(f1)->name, QStringLiteral("主断层"));

    mid.removeFault(f1); // 第二轮编辑落盘
    QVERIFY(store.save(mid));

    FaultSet final;
    FaultSetStore reader2(dbPath());
    reader2.open(nullptr);
    reader2.load(final);
    QCOMPARE(final.faultCount(), 0);
}

QTEST_MAIN(TestFaultSetStore)
#include "tst_faultsetstore.moc"
