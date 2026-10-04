// 方向 30：services/cataloghealth 召回测试——人工植入 缺失文件 / SHA 失配 /
// 未决链接 / 孤立实体 / 无版本资产，快速面全检出且带跳转定位字段；外链 SHA
// 复验检出篡改、取消即停、工程未开如实报错。纯 catalog 栈（无 QGIS init）。
#include <QtTest>
#include <QTemporaryDir>

#include "../src/catalog/datacatalog.h"
#include "../src/services/cataloghealth.h"

using namespace paleo::health;

class TestCatalogHealth : public QObject
{
  Q_OBJECT

  static CatalogEntity well(const QString &id, const QString &name)
  {
    CatalogEntity e;
    e.id = id;
    e.entityType = QStringLiteral("well");
    e.name = name;
    return e;
  }

  static CatalogAsset asset(const QString &id, const QString &type,
                            const QString &displayName)
  {
    CatalogAsset a;
    a.id = id;
    a.type = type;
    a.format = QStringLiteral("dat");
    a.displayName = displayName;
    return a;
  }

  static CatalogVersion managedVersion(const QString &id, const QString &assetId,
                                       const QString &relPath, const QString &sha)
  {
    CatalogVersion v;
    v.id = id;
    v.assetId = assetId;
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = true;
    v.path = relPath;
    v.sha256 = sha;
    v.fileName = QFileInfo(relPath).fileName();
    return v;
  }

private slots:
  void quickReportDetectsAllPlantedIssues()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY(cat.open(tmp.path(), &err));

    // 好资产：受管文件在盘、链接已决。
    QVERIFY(cat.addEntity(well(QStringLiteral("well-1"), QStringLiteral("A1")), &err));
    QVERIFY(cat.addAsset(asset(QStringLiteral("ast-1"), QStringLiteral("well_head"),
                               QStringLiteral("A1 井口")), &err));
    const QString rel1 = QStringLiteral("artifacts/raw/ast-1/ver-1/A1.dat");
    const QString abs1 = QDir(tmp.path()).filePath(rel1);
    QVERIFY(QDir().mkpath(QFileInfo(abs1).absolutePath()));
    QFile f1(abs1);
    QVERIFY(f1.open(QIODevice::WriteOnly));
    f1.write("head");
    f1.close();
    QVERIFY(cat.addVersion(managedVersion(QStringLiteral("ver-1"), QStringLiteral("ast-1"),
                                          rel1, QString()),
                           &err));
    EntityAssetLink okLink;
    okLink.entityType = QStringLiteral("well");
    okLink.entityId = QStringLiteral("well-1");
    okLink.assetId = QStringLiteral("ast-1");
    okLink.role = QStringLiteral("well_head");
    QVERIFY(cat.addLink(okLink, &err));

    // 植入 1：受管版本文件缺失（行在盘上无）。
    QVERIFY(cat.addAsset(asset(QStringLiteral("ast-3"), QStringLiteral("horizon"),
                               QStringLiteral("幽灵层位")), &err));
    QVERIFY(cat.addVersion(managedVersion(
                               QStringLiteral("ver-3"), QStringLiteral("ast-3"),
                               QStringLiteral("artifacts/raw/ast-3/ver-3/h.dat"), QString()),
                           &err));

    // 植入 2：未决链接（外链资产 ast-2，entityId 留空）。
    QVERIFY(cat.addAsset(asset(QStringLiteral("ast-2"), QStringLiteral("seismic"),
                               QStringLiteral("三维体（外链）")), &err));
    const QString extPath = QDir(tmp.path()).filePath(QStringLiteral("ext.sgy"));
    {
      QFile f(extPath);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(QByteArray(64, '\x1'));
    }
    QString serr;
    const QString extSha = DataCatalog::sha256FileHex(extPath, &serr);
    QVERIFY(!extSha.isEmpty());
    CatalogVersion ext;
    ext.id = QStringLiteral("ver-2");
    ext.assetId = QStringLiteral("ast-2");
    ext.stage = QStringLiteral("RAW");
    ext.versionNumber = 1;
    ext.managed = false;
    ext.path = extPath;
    ext.sha256 = extSha;
    ext.fileName = QStringLiteral("ext.sgy");
    QVERIFY(cat.addVersion(ext, &err));
    EntityAssetLink pending;
    pending.entityType = QStringLiteral("well");
    pending.assetId = QStringLiteral("ast-2");
    pending.role = QStringLiteral("well_log"); // 与 entityType 匹配，避免词表违例混入计数
    pending.unresolved = true;
    pending.note = QStringLiteral("未匹配井名: B9");
    QVERIFY(cat.addLink(pending, &err));

    // 植入 3：孤立实体（零链接井）。
    QVERIFY(cat.addEntity(well(QStringLiteral("well-2"), QStringLiteral("LONELY")), &err));

    // 植入 4：无版本资产。
    QVERIFY(cat.addAsset(asset(QStringLiteral("ast-4"), QStringLiteral("document"),
                               QStringLiteral("空壳文档")), &err));

    QString herr;
    const HealthReport rep = buildCatalogHealth(&cat, tmp.path(), &herr);
    QVERIFY(herr.isEmpty());
    QCOMPARE(rep.count(IssueKind::MissingFile), 1);
    QCOMPARE(rep.count(IssueKind::PendingLink), 1);
    QCOMPARE(rep.count(IssueKind::OrphanEntity), 1);
    QCOMPARE(rep.count(IssueKind::NoVersionAsset), 1);
    QCOMPARE(rep.count(IssueKind::InvalidRoleLink), 0);

    // 跳转定位字段：缺失文件带 assetId+versionId；未决带 assetId；孤立带 entityId。
    bool sawMissing = false, sawPending = false, sawOrphan = false;
    for (const HealthIssue &i : rep.issues)
    {
      if (i.kind == IssueKind::MissingFile)
      {
        QCOMPARE(i.assetId, QStringLiteral("ast-3"));
        QCOMPARE(i.versionId, QStringLiteral("ver-3"));
        sawMissing = true;
      }
      if (i.kind == IssueKind::PendingLink)
      {
        QCOMPARE(i.assetId, QStringLiteral("ast-2"));
        QVERIFY(i.detail.contains(QStringLiteral("B9")));
        sawPending = true;
      }
      if (i.kind == IssueKind::OrphanEntity)
      {
        QCOMPARE(i.entityId, QStringLiteral("well-2"));
        sawOrphan = true;
      }
    }
    QVERIFY(sawMissing);
    QVERIFY(sawPending);
    QVERIFY(sawOrphan);

    // 外链源被删 → 快速面把它记为缺失文件。
    QVERIFY(QFile::remove(extPath));
    const HealthReport rep2 = buildCatalogHealth(&cat, tmp.path(), &herr);
    QCOMPARE(rep2.count(IssueKind::MissingFile), 2);
  }

  void shaVerifyDetectsTamperAndCancelStops()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    DataCatalog cat;
    QString err;
    QVERIFY(cat.open(tmp.path(), &err));
    QVERIFY(cat.addAsset(asset(QStringLiteral("ast-9"), QStringLiteral("seismic"),
                               QStringLiteral("外链体")), &err));
    const QString p = QDir(tmp.path()).filePath(QStringLiteral("e2.sgy"));
    {
      QFile f(p);
      f.open(QIODevice::WriteOnly);
      f.write(QByteArray(32, '\x2'));
    }
    QString serr;
    const QString sha = DataCatalog::sha256FileHex(p, &serr);
    CatalogVersion v;
    v.id = QStringLiteral("ver-9");
    v.assetId = QStringLiteral("ast-9");
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = false;
    v.path = p;
    v.sha256 = sha;
    v.fileName = QStringLiteral("e2.sgy");
    QVERIFY(cat.addVersion(v, &err));

    // 内容未变 → 零问题。
    QVector<HealthIssue> issues = verifyExternalShas(cat.versions());
    QVERIFY(issues.isEmpty());

    // 篡改源文件 → 检出 1 条，带 assetId/versionId。
    {
      QFile f(p);
      QVERIFY(f.open(QIODevice::Append));
      f.write("tampered");
    }
    issues = verifyExternalShas(cat.versions());
    QCOMPARE(issues.size(), 1);
    QCOMPARE(issues.first().kind, IssueKind::ShaMismatch);
    QCOMPARE(issues.first().assetId, QStringLiteral("ast-9"));
    QCOMPARE(issues.first().versionId, QStringLiteral("ver-9"));

    // 立即取消 → 零输出（未扫完语义由调用方自标）。
    issues = verifyExternalShas(cat.versions(), [](int, int, const QString &) { return false; });
    QVERIFY(issues.isEmpty());

    // 进度回调逐版本到达。
    int calls = 0;
    verifyExternalShas(cat.versions(), [&](int, int, const QString &) {
      ++calls;
      return true;
    });
    QCOMPARE(calls, 1);
  }

  void closedCatalogIsHonestError()
  {
    DataCatalog cat; // 未 open
    QString err;
    const HealthReport rep = buildCatalogHealth(&cat, QStringLiteral("/nonexistent"), &err);
    QVERIFY(rep.isEmpty());
    QVERIFY(!err.isEmpty());
  }
};

QTEST_MAIN(TestCatalogHealth)
#include "tst_cataloghealth.moc"
