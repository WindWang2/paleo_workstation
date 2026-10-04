// 方向 30：workflow/assetops 契约测试——版本回滚（新版本指向旧内容、SHA 一致、
// 历史保留、篡改拒绝）、物理清理（磁盘+catalog 双清、血缘保护、外链源不碰）、
// 未决链接批量归位（与导入同一判据：恰一候选才挂）。纯 catalog 栈。
#include <QtTest>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/workflow/assetops.h"

using namespace paleo::assetops;

class TestAssetOps : public QObject
{
  Q_OBJECT

  static bool writeFile(const QString &path, const QByteArray &bytes)
  {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
      return false;
    f.write(bytes);
    f.close();
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadUser |
                                    QFileDevice::ReadGroup | QFileDevice::ReadOther);
    return true;
  }

  // 建资产 + 两个受管版本（真实文件在盘）。
  struct ManagedAsset
  {
    QString assetId;
    QString v1, v2;         // 版本 id
    QString v1Sha, v2Sha;
    QString v1Abs, v2Abs;
  };

  // 建资产 + 两个受管版本（真实文件在盘）。失败 → false（调用方 QVERIFY）。
  static bool makeManagedAsset(DataCatalog &cat, const QDir &dir,
                               const QString &assetId, ManagedAsset *out)
  {
    ManagedAsset m;
    m.assetId = assetId;
    CatalogAsset a;
    a.id = assetId;
    a.type = QStringLiteral("well_log");
    a.format = QStringLiteral("las");
    a.displayName = assetId + QStringLiteral(".Las");
    QString err;
    if (!cat.addAsset(a, &err))
      return false;

    m.v1 = cat.nextVersionId();
    m.v2 = cat.nextVersionId();
    const QString rel1 = DataCatalog::managedPath(QStringLiteral("RAW"), assetId, m.v1,
                                                  QStringLiteral("a.las"));
    const QString rel2 = DataCatalog::managedPath(QStringLiteral("RAW"), assetId, m.v2,
                                                  QStringLiteral("a.las"));
    m.v1Abs = dir.filePath(rel1);
    m.v2Abs = dir.filePath(rel2);
    if (!writeFile(m.v1Abs, QByteArray("VERSION ONE\nCURVE\n")))
      return false;
    if (!writeFile(m.v2Abs, QByteArray("VERSION TWO COMPLETELY DIFFERENT\n")))
      return false;
    m.v1Sha = DataCatalog::sha256FileHex(m.v1Abs, &err);
    m.v2Sha = DataCatalog::sha256FileHex(m.v2Abs, &err);
    if (m.v1Sha.isEmpty() || m.v2Sha.isEmpty())
      return false;

    CatalogVersion ver1;
    ver1.id = m.v1;
    ver1.assetId = assetId;
    ver1.stage = QStringLiteral("RAW");
    ver1.versionNumber = 1;
    ver1.managed = true;
    ver1.path = rel1;
    ver1.sha256 = m.v1Sha;
    ver1.fileName = QStringLiteral("a.las");
    if (!cat.addVersion(ver1, &err))
      return false;
    CatalogVersion ver2 = ver1;
    ver2.id = m.v2;
    ver2.versionNumber = 2;
    ver2.path = rel2;
    ver2.sha256 = m.v2Sha;
    if (!cat.addVersion(ver2, &err))
      return false;
    *out = m;
    return true;
  }

private slots:
  void rollbackManagedPointsNewVersionAtOldContent()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QDir dir(tmp.path());
    DataCatalog cat;
    QString err;
    QVERIFY(cat.open(dir.absolutePath(), &err));
    ManagedAsset m;
    QVERIFY(makeManagedAsset(cat, dir, QStringLiteral("ast-1"), &m));

    QString rerr;
    const RollbackOutcome out = rollbackToVersion(&cat, dir.absolutePath(),
                                                  m.assetId, m.v1, &rerr);
    QVERIFY2(!out.versionId.isEmpty(), qPrintable(rerr));
    QCOMPARE(out.versionNumber, 3);
    QCOMPARE(out.sha256, m.v1Sha);

    // 历史全保留 + 新版本成为 currentVersion。
    const QVector<CatalogVersion> versions = cat.versionsForAsset(m.assetId);
    QCOMPARE(versions.size(), 3);
    const CatalogVersion cur = cat.currentVersion(m.assetId);
    QCOMPARE(cur.id, out.versionId);
    QCOMPARE(cur.versionNumber, 3);
    QCOMPARE(cur.sha256, m.v1Sha);
    QCOMPARE(cur.parentVersionIds, QStringList{m.v1});
    QCOMPARE(cur.extra.value(QStringLiteral("rollbackOf")).toString(), m.v1);

    // 文件字节 == 目标版本内容（不是 v2）。
    QFile f(cat.resolvedVersionPath(dir.absolutePath(), cur));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("VERSION ONE\nCURVE\n"));
    QVERIFY(QFileInfo(f.fileName()).permission(QFile::ReadOwner));
    QVERIFY(!QFileInfo(f.fileName()).permission(QFile::WriteOwner)); // 只读纪律

    // 回滚到当前版本 → 拒绝。
    QString e2;
    QVERIFY(rollbackToVersion(&cat, dir.absolutePath(), m.assetId, cur.id, &e2).versionId.isEmpty());
    QVERIFY(!e2.isEmpty());
    // 别的资产的版本 id → 拒绝。
    QVERIFY(rollbackToVersion(&cat, dir.absolutePath(), QStringLiteral("ast-other"), m.v1, &e2)
                .versionId.isEmpty());
  }

  void rollbackRejectsTamperedTarget()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QDir dir(tmp.path());
    DataCatalog cat;
    QString err;
    QVERIFY(cat.open(dir.absolutePath(), &err));
    ManagedAsset m;
    QVERIFY(makeManagedAsset(cat, dir, QStringLiteral("ast-1"), &m));

    // 篡改 v1 盘上字节（目录夹带写权限）。
    QFile::setPermissions(m.v1Abs, QFile::permissions(m.v1Abs) | QFileDevice::WriteOwner);
    {
      QFile f(m.v1Abs);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("EVIL CONTENT");
    }

    QString rerr;
    const RollbackOutcome out = rollbackToVersion(&cat, dir.absolutePath(),
                                                  m.assetId, m.v1, &rerr);
    QVERIFY(out.versionId.isEmpty());
    QVERIFY(rerr.contains(QStringLiteral("SHA-256")));
    QCOMPARE(cat.versionsForAsset(m.assetId).size(), 2); // 未新增版本行
  }

  void rollbackExternalIsZeroCopy()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QDir dir(tmp.path());
    DataCatalog cat;
    QString err;
    QVERIFY(cat.open(dir.absolutePath(), &err));
    CatalogAsset a;
    a.id = QStringLiteral("ast-x");
    a.type = QStringLiteral("seismic");
    a.format = QStringLiteral("sgy");
    a.displayName = QStringLiteral("ext.sgy");
    QVERIFY(cat.addAsset(a, &err));
    const QString p1 = dir.filePath(QStringLiteral("one.sgy"));
    const QString p2 = dir.filePath(QStringLiteral("two.sgy"));
    QVERIFY(writeFile(p1, QByteArray("ONE")));
    QVERIFY(writeFile(p2, QByteArray("TWO")));
    const QString sha1 = DataCatalog::sha256FileHex(p1, &err);
    CatalogVersion v1;
    v1.id = QStringLiteral("vx1");
    v1.assetId = a.id;
    v1.stage = QStringLiteral("RAW");
    v1.versionNumber = 1;
    v1.managed = false;
    v1.path = p1;
    v1.sha256 = sha1;
    v1.fileName = QStringLiteral("one.sgy");
    QVERIFY(cat.addVersion(v1, &err));
    CatalogVersion v2 = v1;
    v2.id = QStringLiteral("vx2");
    v2.versionNumber = 2;
    v2.path = p2;
    v2.sha256 = DataCatalog::sha256FileHex(p2, &err);
    v2.fileName = QStringLiteral("two.sgy");
    QVERIFY(cat.addVersion(v2, &err));

    QString rerr;
    const RollbackOutcome out = rollbackToVersion(&cat, dir.absolutePath(), a.id,
                                                  QStringLiteral("vx1"), &rerr);
    QVERIFY2(!out.versionId.isEmpty(), qPrintable(rerr));
    const CatalogVersion cur = cat.currentVersion(a.id);
    QVERIFY(!cur.managed);
    QCOMPARE(cur.path, p1);       // 指向目标源路径
    QCOMPARE(cur.sha256, sha1);   // 留底一致
    QCOMPARE(cat.versionsForAsset(a.id).size(), 3);
  }

  void compareVersionsReportsMetaAndTextDiff()
  {
    QTemporaryDir tmp;
    QDir dir(tmp.path());
    DataCatalog cat;
    QString err;
    QVERIFY(cat.open(dir.absolutePath(), &err));
    ManagedAsset m;
    QVERIFY(makeManagedAsset(cat, dir, QStringLiteral("ast-1"), &m));
    const CatalogVersion v1 = cat.versionById(m.v1);
    const CatalogVersion v2 = cat.versionById(m.v2);

    const VersionCompare c = compareVersions(dir.absolutePath(), v1, v2);
    QVERIFY(c.shaKnown);
    QVERIFY(!c.sameSha);
    QCOMPARE(c.sizeA, QFileInfo(m.v1Abs).size());
    QCOMPARE(c.sizeB, QFileInfo(m.v2Abs).size());
    QVERIFY(c.textCompared); // las 属文本白名单
    QVERIFY(c.linesA > 0 || c.linesB > 0);
    QVERIFY(!c.differingLines.isEmpty());

    const VersionCompare same = compareVersions(dir.absolutePath(), v1, v1);
    QVERIFY(same.sameSha);
    QVERIFY(same.fieldDiffs.isEmpty());
    QVERIFY(same.differingLines.isEmpty());
  }

  void purgeDoubleClearsAndGuardsLineage()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QDir dir(tmp.path());
    DataCatalog cat;
    QString err;
    QVERIFY(cat.open(dir.absolutePath(), &err));
    ManagedAsset raw;
    QVERIFY(makeManagedAsset(cat, dir, QStringLiteral("ast-raw"), &raw));

    // 派生资产引用 raw v1 → raw 受血缘保护。
    CatalogAsset derived;
    derived.id = QStringLiteral("ast-der");
    derived.type = QStringLiteral("thickness");
    derived.format = QStringLiteral("tif");
    derived.displayName = QStringLiteral("thickness.tif");
    QVERIFY(cat.addAsset(derived, &err));
    const QString drel = DataCatalog::managedPath(QStringLiteral("DERIVED"), derived.id,
                                                  QStringLiteral("ver-d"),
                                                  QStringLiteral("thickness.tif"));
    const QString dabs = dir.filePath(drel);
    QVERIFY(writeFile(dabs, QByteArray("DERIVED BYTES")));
    CatalogVersion dv;
    dv.id = QStringLiteral("ver-d");
    dv.assetId = derived.id;
    dv.stage = QStringLiteral("DERIVED");
    dv.versionNumber = 1;
    dv.managed = true;
    dv.path = drel;
    dv.sha256 = DataCatalog::sha256FileHex(dabs, &err);
    dv.fileName = QStringLiteral("thickness.tif");
    dv.parentVersionIds = QStringList{raw.v1};
    QVERIFY(cat.addVersion(dv, &err));

    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    link.entityId = QStringLiteral("w-nope"); // 无实体也允许（链接表自便）
    link.assetId = raw.assetId;
    link.role = QStringLiteral("well_log");
    QVERIFY(cat.addLink(link, &err));

    // 血缘保护：raw 删不掉。
    PurgeOutcome p1 = purgeAssets(&cat, dir.absolutePath(), {raw.assetId});
    QCOMPARE(p1.purgedAssetIds.size(), 0);
    QVERIFY(p1.failedAssets.size() == 1);
    QVERIFY(QFileInfo::exists(raw.v1Abs));

    // 先删派生再删 raw → 双清。
    PurgeOutcome p2 = purgeAssets(&cat, dir.absolutePath(), {derived.id});
    QCOMPARE(p2.purgedAssetIds, QStringList{derived.id});
    QVERIFY(!QFileInfo::exists(dabs));
    QVERIFY(p2.bytesFreed > 0);
    QVERIFY(cat.assetById(derived.id).id.isEmpty());

    PurgeOutcome p3 = purgeAssets(&cat, dir.absolutePath(), {raw.assetId});
    QCOMPARE(p3.purgedAssetIds, QStringList{raw.assetId});
    QVERIFY(!QFileInfo::exists(raw.v1Abs));
    QVERIFY(!QFileInfo::exists(raw.v2Abs));
    QVERIFY(cat.assetById(raw.assetId).id.isEmpty());
    QVERIFY(cat.versionsForAsset(raw.assetId).isEmpty());
    QVERIFY(cat.linksForAsset(raw.assetId).isEmpty());
    QVERIFY(p3.bytesFreed > 0);

    // 重开 catalog：sqlite 侧同样双清。
    DataCatalog cat2;
    QVERIFY(cat2.open(dir.absolutePath(), &err));
    QVERIFY(cat2.assetById(raw.assetId).id.isEmpty());
    QVERIFY(cat2.versions().isEmpty());
    QVERIFY(cat2.links().isEmpty());
  }

  void pendingResolveProposesExactlyOneMatchOnly()
  {
    QTemporaryDir tmp;
    QDir dir(tmp.path());
    DataCatalog cat;
    QString err;
    QVERIFY(cat.open(dir.absolutePath(), &err));

    auto addWell = [&cat, &err](const QString &id, const QString &name) {
      CatalogEntity e;
      e.id = id;
      e.entityType = QStringLiteral("well");
      e.name = name;
      QVERIFY2(cat.addEntity(e, &err), qPrintable(err));
    };
    addWell(QStringLiteral("w1"), QStringLiteral("A1"));
    addWell(QStringLiteral("w2"), QStringLiteral("B-1"));
    addWell(QStringLiteral("w3"), QStringLiteral("B 1")); // 归一化后与 B-1 同名 → 歧义

    auto addPendingLog = [&cat, &err, &dir](const QString &assetId, const QString &fileName,
                                      const QString &note) {
      CatalogAsset a;
      a.id = assetId;
      a.type = QStringLiteral("well_log");
      a.format = QStringLiteral("las");
      a.displayName = fileName;
      QVERIFY2(cat.addAsset(a, &err), qPrintable(err));
      const QString rel = DataCatalog::managedPath(QStringLiteral("RAW"), assetId,
                                                   QStringLiteral("v-") + assetId, fileName);
      writeFile(dir.filePath(rel), QByteArray("~LOG\n"));
      CatalogVersion v;
      v.id = QStringLiteral("v-") + assetId;
      v.assetId = assetId;
      v.stage = QStringLiteral("RAW");
      v.versionNumber = 1;
      v.managed = true;
      v.path = rel;
      v.sha256 = DataCatalog::sha256FileHex(dir.filePath(rel), &err);
      v.fileName = fileName;
      QVERIFY2(cat.addVersion(v, &err), qPrintable(err));
      EntityAssetLink l;
      l.entityType = QStringLiteral("well");
      l.assetId = assetId;
      l.role = QStringLiteral("well_log");
      l.unresolved = true;
      l.note = note;
      QVERIFY2(cat.addLink(l, &err), qPrintable(err));
    };

    // A1.Las → 恰一候选（w1）；B1.Las → 两候选（w2/w3 归一化同名）；C9.Las → 零。
    addPendingLog(QStringLiteral("pa"), QStringLiteral("A1.Las"),
                  QStringLiteral("未匹配井名: A1"));
    addPendingLog(QStringLiteral("pb"), QStringLiteral("B1.Las"),
                  QStringLiteral("候选: b-1, b 1"));
    addPendingLog(QStringLiteral("pc"), QStringLiteral("C9.Las"),
                  QStringLiteral("未匹配井名: C9"));

    const QVector<PendingProposal> props = proposablePendingLinks(&cat);
    QCOMPARE(props.size(), 1);
    QCOMPARE(props.first().assetId, QStringLiteral("pa"));
    QCOMPARE(props.first().wellId, QStringLiteral("w1"));
    QVERIFY(props.first().linkIndex >= 0);

    // 应用后：pa 挂到 w1；其余未决仍在。
    QString aerr;
    const int n = applyPendingResolutions(&cat, {props.first().linkIndex}, &aerr);
    QCOMPARE(n, 1);
    QCOMPARE(cat.unresolvedLinks().size(), 2);
    bool attached = false;
    for (const EntityAssetLink &l : cat.linksForAsset(QStringLiteral("pa")))
      if (!l.unresolved && l.entityId == QStringLiteral("w1"))
        attached = true;
    QVERIFY(attached);

    // 主关联已占：w1 的 well_log 已有已决主关联——同角色另一未决不再提议。
    addPendingLog(QStringLiteral("pd"), QStringLiteral("A1-2.Las"),
                  QStringLiteral("未匹配井名: A1"));
    // 文件名主名 A1-2 归一化 a12 ≠ a1，走 note 兜底 A1 → 但主位已占 → 不提议。
    const QVector<PendingProposal> props2 = proposablePendingLinks(&cat);
    for (const PendingProposal &p : props2)
      QVERIFY(p.assetId != QStringLiteral("pd"));
  }
};

QTEST_MAIN(TestAssetOps)
#include "tst_assetops.moc"
