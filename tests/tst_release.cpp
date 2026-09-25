#include <QtTest>
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
};

QTEST_GUILESS_MAIN(TestReleaseStore)
#include "tst_release.moc"
