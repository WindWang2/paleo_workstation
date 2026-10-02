// #84：「同 SHA 已入库」复核在目录导入（CatalogReadSnapshot）与单文件导入
//（DataCatalog）两条入口上必须同一语义——真重哈希库内副本。受管副本被保
// mtime/size 改写后，两条入口都不得判为已入库。
#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/io/ingestplan.h"

class TestSnapshotDedup : public QObject
{
  Q_OBJECT

private slots:
  void tamperedManagedCopyIsNotADuplicateOnEitherPath()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY2(cat.open(dir.path(), &err), qPrintable(err));

    // 受管副本：工程相对 RAW/<asset>/<version>/<file>。
    const QString rel = QStringLiteral("RAW/asset-1/ver-1/w1.las");
    const QString abs = QDir(dir.path()).filePath(rel);
    QVERIFY(QDir().mkpath(QFileInfo(abs).absolutePath()));
    const QByteArray original("~Version\nVERS. 2.0 :\n~A\n1000 1\n");
    {
      QFile f(abs);
      QVERIFY(f.open(QIODevice::WriteOnly));
      QCOMPARE(f.write(original), qint64(original.size()));
    }
    const QString sha = DataCatalog::sha256FileHex(abs, &err);
    QVERIFY2(!sha.isEmpty(), qPrintable(err));

    CatalogAsset a;
    a.id = QStringLiteral("asset-1");
    a.type = QStringLiteral("well_log");
    a.format = QStringLiteral("las");
    a.displayName = QStringLiteral("w1");
    QVERIFY2(cat.addAsset(a, &err), qPrintable(err));
    CatalogVersion v;
    v.id = QStringLiteral("ver-1");
    v.assetId = a.id;
    v.stage = QStringLiteral("RAW");
    v.managed = true;
    v.path = rel;
    v.sha256 = sha;
    v.fileName = QStringLiteral("w1.las");
    QVERIFY2(cat.addVersion(v, &err), qPrintable(err));

    // 基线：两条入口都命中（同时把指纹写进 ShaCache——旧实现之后就只看指纹）。
    const CatalogReadSnapshot snap = CatalogReadSnapshot::fromCatalog(cat);
    QCOMPARE(cat.versionBySha256(sha).id, v.id);
    QCOMPARE(snap.versionBySha256(sha).id, v.id);

    // 同长度改写内容，并把 mtime 拨回原值（保时间戳工具 / touch -r / 位腐）。
    const QDateTime mtime = QFileInfo(abs).lastModified();
    {
      QFile f(abs);
      QVERIFY(f.open(QIODevice::ReadWrite));
      QByteArray tampered = original;
      tampered[tampered.size() - 2] = '9';
      QCOMPARE(f.write(tampered), qint64(tampered.size()));
      f.close();
      QVERIFY(f.open(QIODevice::ReadWrite));
      QVERIFY(f.setFileTime(mtime, QFileDevice::FileModificationTime));
    }
    QCOMPARE(QFileInfo(abs).size(), qint64(original.size()));
    QCOMPARE(QFileInfo(abs).lastModified(), mtime);

    QVERIFY2(cat.versionBySha256(sha).id.isEmpty(), "live catalog must reject tampered copy");
    QVERIFY2(snap.versionBySha256(sha).id.isEmpty(),
             "snapshot must agree with live catalog (re-hash, not fingerprint)");
  }
};

QTEST_GUILESS_MAIN(TestSnapshotDedup)
#include "tst_snapshot_dedup.moc"
