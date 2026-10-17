#include <QtTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include "../src/metadata/releasestore.h"

// Release semantics: named, immutable snapshots of the declared layer set in
// the project sqlite — listing, manifest recovery, and release-to-release diff.
class TestReleaseStore : public QObject
{
  Q_OBJECT

private:
  static LayerDeclaration decl(const QString &id, const QString &horizon,
                               const QString &source = QStringLiteral("memory|none"))
  {
    LayerDeclaration d;
    d.layerId = id;
    d.horizon = horizon;
    d.type = QStringLiteral("vector");
    d.source = source;
    d.styleRef = QStringLiteral("styles/%1.qml").arg(id);
    d.group = QStringLiteral("04_SingleFactor");
    return d;
  }

private slots:

  void createAndListReleases()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ReleaseStore store(dir.filePath(QStringLiteral("meta.sqlite")));
    QVERIFY(store.open());

    const QString r1 = store.createRelease(QStringLiteral("v0-draft"),
                                           QStringLiteral("first cut"),
                                           {decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))});
    QVERIFY(!r1.isEmpty());
    const QString r2 = store.createRelease(
        QStringLiteral("v1"), QString(),
        {decl(QStringLiteral("facies.T1"), QStringLiteral("T1")),
         decl(QStringLiteral("facies.T2"), QStringLiteral("T2"))});
    QVERIFY(!r2.isEmpty());
    QVERIFY(r1 != r2);

    const auto list = store.releases();
    QCOMPARE(list.size(), 2);
    QCOMPARE(list.at(0).id, r1); // chronological: rel-1 then rel-2
    QCOMPARE(list.at(0).name, QStringLiteral("v0-draft"));
    QCOMPARE(list.at(0).layerCount, 1);
    QCOMPARE(list.at(1).layerCount, 2);
    QVERIFY(!list.at(0).createdUtc.isEmpty());
  }

  void manifestAtRoundTrips()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ReleaseStore store(dir.filePath(QStringLiteral("meta.sqlite")));
    QVERIFY(store.open());

    const QVector<LayerDeclaration> decls = {
        decl(QStringLiteral("facies.T1"), QStringLiteral("T1"),
             QStringLiteral("/data/a.gpkg|layername=basin")),
        decl(QStringLiteral("faults.T2"), QStringLiteral("T2")),
    };
    const QString id = store.createRelease(QStringLiteral("snap"), QString(), decls);
    QVERIFY(!id.isEmpty());

    const auto got = store.manifestAt(id);
    QCOMPARE(got.size(), 2);
    QCOMPARE(got.at(0).layerId, QStringLiteral("facies.T1"));
    QCOMPARE(got.at(0).source, QStringLiteral("/data/a.gpkg|layername=basin"));
    QCOMPARE(got.at(1).horizon, QStringLiteral("T2"));
    QCOMPARE(got.at(0).instantiated, false);
  }

  // opt-05：发布快照必须保住显示名——ManifestProjection 的内嵌格式写 title，
  // 发布清单掉 title 会让 manifestAt 读回的声明丢树名/图例名，diff 对纯改名
  // 也失明（manifestKey 不含 title 时同名不同标题比较为相等）。
  void manifestAtRoundTripsTitle()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ReleaseStore store(dir.filePath(QStringLiteral("meta.sqlite")));
    QVERIFY(store.open());

    LayerDeclaration named =
        decl(QStringLiteral("facies.T1"), QStringLiteral("T1"));
    named.title = QStringLiteral("T1 沉积相图");
    const QString id =
        store.createRelease(QStringLiteral("snap"), QString(), {named});
    QVERIFY(!id.isEmpty());

    const auto got = store.manifestAt(id);
    QCOMPARE(got.size(), 1);
    QCOMPARE(got.at(0).title, QStringLiteral("T1 沉积相图"));
  }

  // opt-05：纯显示名变更（同 layerId、同 source）也是声明差异，diff 必须
  // 报 changed——否则图名/图例名修正对版本对比不可见。
  void diffFlagsTitleOnlyChange()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ReleaseStore store(dir.filePath(QStringLiteral("meta.sqlite")));
    QVERIFY(store.open());

    LayerDeclaration a = decl(QStringLiteral("facies.T1"), QStringLiteral("T1"));
    a.title = QStringLiteral("旧图名");
    LayerDeclaration b = a;
    b.title = QStringLiteral("新图名");
    const QString ra =
        store.createRelease(QStringLiteral("a"), QString(), {a});
    const QString rb =
        store.createRelease(QStringLiteral("b"), QString(), {b});
    QVERIFY(!ra.isEmpty() && !rb.isEmpty());

    QStringList added, removed, changed;
    QVERIFY(store.diff(ra, rb, &added, &removed, &changed));
    QVERIFY(added.isEmpty() && removed.isEmpty());
    QCOMPARE(changed, QStringList{QStringLiteral("facies.T1")});
  }

  // opt-05：manifest_json 损坏时 diff 不得把「读不出」谎报成「对方全新增」
  // ——返回 false 且点名损坏的 release。空数组是合法快照不拦。
  void diffRefusesCorruptManifest()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("meta.sqlite"));
    QString a, b;
    {
      ReleaseStore store(dbPath);
      QVERIFY(store.open());
      a = store.createRelease(QStringLiteral("a"), QString(),
                              {decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))});
      b = store.createRelease(QStringLiteral("b"), QString(),
                              {decl(QStringLiteral("grid.T1"), QStringLiteral("T1"))});
      QVERIFY(!a.isEmpty() && !b.isEmpty());
    }
    {
      QSqlDatabase db = QSqlDatabase::addDatabase(
          QStringLiteral("QSQLITE"), QStringLiteral("corrupt_fixture"));
      db.setDatabaseName(dbPath);
      QVERIFY(db.open());
      QSqlQuery q(db);
      QVERIFY(q.exec(QStringLiteral(
          "UPDATE releases SET manifest_json='@@@' WHERE release_id='%1'").arg(a)));
      db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("corrupt_fixture"));

    ReleaseStore store(dbPath);
    QVERIFY(store.open());
    QStringList added, removed, changed;
    QString err;
    QVERIFY(!store.diff(a, b, &added, &removed, &changed, &err));
    QVERIFY2(err.contains(a), qPrintable(err));

    // 另一侧损坏同样点名；两侧都损坏点第一个。
    {
      QSqlDatabase db = QSqlDatabase::addDatabase(
          QStringLiteral("QSQLITE"), QStringLiteral("corrupt_fixture2"));
      db.setDatabaseName(dbPath);
      QVERIFY(db.open());
      QSqlQuery q(db);
      QVERIFY(q.exec(QStringLiteral(
          "UPDATE releases SET manifest_json='@@@' WHERE release_id='%1'").arg(b)));
      db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("corrupt_fixture2"));
    err.clear();
    QVERIFY(!store.diff(a, b, &added, &removed, &changed, &err));
    QVERIFY2(err.contains(a), qPrintable(err));

    // 列表口径同步：损坏快照在 releases() 里如实标 manifestCorrupt，
    // 不谎报 layerCount=0（面板上显示「损坏」而非静默当空清单）。
    const QVector<ReleaseInfo> infos = store.releases();
    const auto findInfo = [&infos](const QString &id) {
      for (const ReleaseInfo &r : infos)
        if (r.id == id)
          return r;
      return ReleaseInfo{};
    };
    QVERIFY(findInfo(a).manifestCorrupt);
    QVERIFY(findInfo(b).manifestCorrupt);
  }

  void diffReportsDelta()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ReleaseStore store(dir.filePath(QStringLiteral("meta.sqlite")));
    QVERIFY(store.open());

    const QString a = store.createRelease(QStringLiteral("a"), QString(),
        {decl(QStringLiteral("facies.T1"), QStringLiteral("T1")),
         decl(QStringLiteral("faults.T1"), QStringLiteral("T1"))});
    // B: dropped faults, changed facies source, added grid.
    const QString b = store.createRelease(QStringLiteral("b"), QString(),
        {decl(QStringLiteral("facies.T1"), QStringLiteral("T1"),
              QStringLiteral("/data/b.gpkg|layername=basin")),
         decl(QStringLiteral("grid.T1"), QStringLiteral("T1"))});

    QStringList added, removed, changed;
    QVERIFY(store.diff(a, b, &added, &removed, &changed));
    QCOMPARE(added, QStringList{QStringLiteral("grid.T1")});
    QCOMPARE(removed, QStringList{QStringLiteral("faults.T1")});
    QCOMPARE(changed, QStringList{QStringLiteral("facies.T1")});

    // Unknown ids fail.
    QVERIFY(!store.diff(a, QStringLiteral("rel-999"), nullptr, nullptr, nullptr));
  }

  // 07 方向 Round E：diff 失败原因要可区分（缺哪个版本说清楚）——面板据此
  // 给可行动提示，不再笼统「对比失败」。
  void diffErrorNamesMissingRelease()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ReleaseStore store(dir.filePath(QStringLiteral("meta.sqlite")));
    QVERIFY(store.open());
    const QString a = store.createRelease(QStringLiteral("a"), QString(),
        {decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))});
    QVERIFY(!a.isEmpty());

    QString err;
    QVERIFY(!store.diff(a, QStringLiteral("rel-404"), nullptr, nullptr, nullptr, &err));
    QVERIFY(err.contains(QStringLiteral("rel-404")));
    QVERIFY(err.contains(QStringLiteral("不存在")));

    // 两个都不存在：一并点名。
    QString err2;
    QVERIFY(!store.diff(QStringLiteral("rel-x"), QStringLiteral("rel-y"),
                        nullptr, nullptr, nullptr, &err2));
    QVERIFY(err2.contains(QStringLiteral("rel-x")));
    QVERIFY(err2.contains(QStringLiteral("rel-y")));

    // 同一版本两侧：missing 为空——「两侧同版本」与「不存在」分开说。
    QString errSame;
    QVERIFY(!store.diff(a, a, nullptr, nullptr, nullptr, &errSame));
    QVERIFY(errSame.contains(QStringLiteral("同一个版本")));

    // 成功路径清空 error（调用方可用它判「有没有细节」）——预置 stale 串
    // 验证入口清理而非「从不写」。
    const QString b = store.createRelease(QStringLiteral("b"), QString(),
        {decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))});
    QString err3 = QStringLiteral("stale");
    QStringList added, removed, changed;
    QVERIFY(store.diff(a, b, &added, &removed, &changed, &err3));
    QVERIFY(err3.isEmpty());
  }

  void persistsAcrossStoreInstances()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("meta.sqlite"));

    QString savedId;
    {
      ReleaseStore store(dbPath);
      QVERIFY(store.open());
      savedId = store.createRelease(QStringLiteral("persisted"), QString(),
                                    {decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))});
      QVERIFY(!savedId.isEmpty());
    }
    {
      ReleaseStore store2(dbPath);
      QVERIFY(store2.open());
      const auto list = store2.releases();
      QCOMPARE(list.size(), 1);
      QCOMPARE(list.at(0).name, QStringLiteral("persisted"));
      QCOMPARE(store2.manifestAt(savedId).size(), 1);
    }
  }

  void emptyReleaseIsValid()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ReleaseStore store(dir.filePath(QStringLiteral("meta.sqlite")));
    QVERIFY(store.open());
    const QString id = store.createRelease(QStringLiteral("baseline"), QString(), {});
    QVERIFY(!id.isEmpty());
    QCOMPARE(store.releases().at(0).layerCount, 0);
    QVERIFY(store.manifestAt(id).isEmpty());
  }

  // #80/#476：只读实例通道（LayerManifest 同构）——const 读走独立只读连接
  // 且不建表；库文件不存在时拒开（不在被锁目录里建库）；写方法显式拒绝。
  void readOnlyInstanceReadsButNeverWrites()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("meta.sqlite"));
    {
      ReleaseStore writer(dbPath);
      QVERIFY2(writer.open(), "schema 由写实例负责建");
      const QString id = writer.createRelease(
          QStringLiteral("v1"), QString(),
          {decl(QStringLiteral("facies.T1"), QStringLiteral("T1"))});
      QVERIFY(!id.isEmpty());
    }

    // 库不存在：只读实例拒开并如实报因，不创建文件。
    ReleaseStore absent(dir.filePath(QStringLiteral("absent/meta.sqlite")));
    absent.setReadOnly(true);
    QString absentErr;
    QVERIFY(!absent.open(&absentErr));
    QVERIFY2(absentErr.contains(QStringLiteral("does not exist")),
             qPrintable(absentErr));

    // 已有库：只读连接 const 读正常，写被显式拒绝。
    ReleaseStore ro(dbPath);
    ro.setReadOnly(true);
    QVERIFY(ro.isReadOnly());
    QString openErr;
    QVERIFY2(ro.open(&openErr), qPrintable(openErr));
    QCOMPARE(ro.releases().size(), 1);
    QVERIFY(!ro.manifestAt(QStringLiteral("rel-1")).isEmpty());

    QString writeErr;
    const QString rejected = ro.createRelease(QStringLiteral("v2"), QString(), {},
                                              &writeErr);
    QVERIFY(rejected.isEmpty());
    QVERIFY2(writeErr.contains(QStringLiteral("read-only")), qPrintable(writeErr));
    QCOMPARE(ro.releases().size(), 1); // 库未被写
  }
};

QTEST_GUILESS_MAIN(TestReleaseStore)
#include "tst_release.moc"
