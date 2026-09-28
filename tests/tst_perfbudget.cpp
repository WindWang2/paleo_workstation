#include <QtTest>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/projectdata.h"

// wave3/model-hardening — 非 UI 性能预算（PALEO_QGIS_PLAN §41.6 NFR 可执行化；
// TODOS P1「性能测试」）。只测服务/IO 层，不实例化窗口：
//
//   1. 工程打开：DataCatalog::open(catalog.json) + LayerManifest::readAll，
//      规模 = 20 井 / 40 资产 / ~200 版本 / 40 链接（≈一个层位一轮编图的
//      版本量级；真工区当前 catalog 同数量级）。
//   2. folder 枚举：DataImportService::previewFolder 对 8 目录 × 8 文件
//      （64 文件，带语义目录段）的枚举+分类。
//   3. DataCatalog::save() 在 ~200 版本规模下一次落盘（含 .bak 轮转复制）。
//
// 上限值在量测环境上实测后钉死（宽松 2 倍余量）。
// 量测环境（2026-09-26）：Arch Linux x86_64，linux-lts 6.18，GCC 16.2.1
// release 构建，Qt 6.11.2 offscreen，NVMe 本地盘，构建机空载。
class TestPerfBudget : public QObject
{
  Q_OBJECT

private:
  // 预置一个 ~200 版本规模的工程（一次 BatchSave 落盘，预置本身不计时）。
  static bool seedProject(const QString &projectDir, int nVersions)
  {
    DataCatalog cat;
    QString err;
    if (!cat.open(projectDir, &err))
      return false;
    {
      DataCatalog::BatchSave batch(&cat);
      for (int w = 0; w < 20; ++w)
      {
        CatalogEntity well;
        well.id = QStringLiteral("well-%1").arg(w, 3, 10, QLatin1Char('0'));
        well.entityType = QStringLiteral("well");
        well.name = QStringLiteral("W%1").arg(w);
        well.hasSurface = true;
        well.surfaceX = 1000.0 + w * 25.0;
        well.surfaceY = 5000.0 + w * 25.0;
        well.coordinateStatus = QStringLiteral("untransformed");
        if (!cat.addEntity(well, &err))
          return false;
      }
      const int nAssets = 40;
      for (int a = 0; a < nAssets; ++a)
      {
        CatalogAsset asset;
        asset.id = QStringLiteral("ast-%1").arg(a + 1);
        asset.type = (a % 2 != 0) ? QStringLiteral("well_log") : QStringLiteral("tops");
        asset.format = QStringLiteral("las");
        asset.displayName = QStringLiteral("f%1.Las").arg(a);
        if (!cat.addAsset(asset, &err))
          return false;
      }
      for (int v = 0; v < nVersions; ++v)
      {
        const int assetNo = v % nAssets + 1;
        CatalogVersion ver;
        ver.id = QStringLiteral("ver-%1").arg(v + 1);
        ver.assetId = QStringLiteral("ast-%1").arg(assetNo);
        ver.stage = QStringLiteral("RAW");
        ver.versionNumber = v / nAssets + 1;
        ver.path = QStringLiteral("raw/ast-%1/ver-%1/f%1.Las").arg(v + 1);
        ver.fileName = QStringLiteral("f%1.Las").arg(v);
        if (!cat.addVersion(ver, &err))
          return false;
      }
      for (int l = 0; l < 40; ++l)
      {
        EntityAssetLink link;
        link.entityType = QStringLiteral("well");
        link.entityId = QStringLiteral("well-%1").arg(l % 20, 3, 10, QLatin1Char('0'));
        link.assetId = QStringLiteral("ast-%1").arg(l % 40 + 1);
        link.role = QStringLiteral("well_log");
        if (!cat.addLink(link, &err))
          return false;
      }
      if (!batch.flush(&err))
        return false;
    }

    LayerManifest manifest(QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite")));
    for (int d = 0; d < 12; ++d)
    {
      LayerDeclaration decl;
      decl.layerId = QStringLiteral("facies.T%1").arg(d);
      decl.horizon = QStringLiteral("T%1").arg(d);
      decl.type = QStringLiteral("vector");
      decl.source = QStringLiteral("project.gpkg|layername=facies_T%1").arg(d);
      decl.group = QStringLiteral("05_Paleogeography");
      if (!manifest.upsert(decl, &err))
        return false;
    }
    return true;
  }

  // 复制仓库夹具到 8 个语义目录 × 8 文件 = 64 文件的目录树。
  static bool seedFolderTree(const QTemporaryDir &tmp)
  {
    const QString fixtureDir = QStringLiteral(PROJECT_FIXTURE_DIR);
    const QStringList names = {QStringLiteral("A1.Las"), QStringLiteral("A1_TD.dat"),
                               QStringLiteral("D61_sample.dat"), QStringLiteral("DC.dat"),
                               QStringLiteral("ExportWellHead.dat"), QStringLiteral("facies.geojson"),
                               QStringLiteral("tiny.pdf"), QStringLiteral("mini_seismic.sgy")};
    const QStringList dirs = {QStringLiteral("井位"), QStringLiteral("井分层"),
                              QStringLiteral("时深"), QStringLiteral("层位"),
                              QStringLiteral("井曲线"), QStringLiteral("参考资料"),
                              QStringLiteral("参考相图"), QStringLiteral("misc")};
    for (int d = 0; d < dirs.size(); ++d)
    {
      const QString dirPath = QDir(tmp.path()).filePath(
          QStringLiteral("tree/%1").arg(dirs.at(d)));
      if (!QDir().mkpath(dirPath))
        return false;
      for (int f = 0; f < names.size(); ++f)
      {
        const QString src = QDir(fixtureDir).filePath(names.at(f));
        if (!QFile::copy(src, QDir(dirPath).filePath(
                                  QStringLiteral("r%1_").arg(d) + names.at(f))))
          return false;
      }
    }
    return true;
  }

private slots:
  void initTestCase()
  {
    if (!QgisRuntime::isInitialized())
      QSKIP("QGIS runtime unavailable");
  }

  // 1) 工程打开：catalog.json（~200 版本）+ manifest 全量声明。
  void projectOpenCatalogManifestBudget()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString projectDir = dir.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY2(seedProject(projectDir, 200), "seed failed");

    QElapsedTimer timer;
    timer.start();
    ProjectDataFacade facade;
    QVERIFY2(facade.setProjectDir(projectDir), qPrintable(facade.lastError()));
    LayerManifest manifest(QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite")));
    QVector<LayerDeclaration> decls;
    QVERIFY(manifest.readAll(&decls));
    const qint64 elapsedMs = timer.elapsed();
    QCOMPARE(decls.size(), 12);
    QCOMPARE(facade.wells().size(), 20);

    // 量测基线（环境见文件头）：实测 1–2ms（release，NVMe）。上限钉 100ms
    // ——对 CI 噪声留两个数量级余量，仍能拦住数量级回归。
    qDebug() << "projectOpen(catalog+manifest, 200 versions):" << elapsedMs << "ms (budget 100 ms)";
    QVERIFY2(elapsedMs < 100,
             qPrintable(QStringLiteral("project open took %1 ms > 100 ms budget").arg(elapsedMs)));
  }

  // 2) folder 枚举：previewFolder 只枚举+分类（不导入），64 文件/8 目录。
  void folderEnumerationBudget()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY2(seedFolderTree(dir), "seed tree failed");

    const QString projectDir = dir.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QgisProjectService projectSvc;
    QVERIFY2(projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))),
             qPrintable(projectSvc.lastErrors().join(QLatin1Char(';'))));
    LayerManifest manifest(QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite")));
    QString err;
    QVERIFY(manifest.open(&err));
    QgisLayerService layerSvc(&projectSvc, &manifest);
    PaleoProjectStore store;
    DataImportService importSvc(&layerSvc, &store);
    importSvc.setProjectDir(projectDir);

    // 预热一次：CI 共享 runner 磁盘/页缓存冷态下首次枚举会抖过预算
    QString warmErr;
    importSvc.previewFolder(QDir(dir.path()).filePath(QStringLiteral("tree")), &warmErr);

    QElapsedTimer timer;
    timer.start();
    const QVector<FolderPreviewRow> rows =
        importSvc.previewFolder(QDir(dir.path()).filePath(QStringLiteral("tree")), &err);
    const qint64 elapsedMs = timer.elapsed();
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 64);

    // 量测基线：实测 1ms（64 小文件嗅探）。预热后上限 500ms（两个数量级余量，
    // 容忍 CI 并行负载抖动，仍能拦住病态退化）。
    qDebug() << "folderEnumeration(64 files/8 dirs):" << elapsedMs << "ms (budget 500 ms)";
    QVERIFY2(elapsedMs < 500,
             qPrintable(QStringLiteral("folder enumeration took %1 ms > 500 ms budget").arg(elapsedMs)));
  }

  // 3) save() 在 200 版本规模的一次落盘（QSaveFile 全量序列化 + .bak 轮转复制）。
  void catalogSaveBudget200Versions()
  {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString projectDir = dir.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY2(seedProject(projectDir, 200), "seed failed");

    DataCatalog cat;
    QString err;
    QVERIFY(cat.open(projectDir, &err));
    // 200 版本轮转在 40 资产上 → 每资产 5 版。
    QCOMPARE(cat.versionsForAsset(QStringLiteral("ast-1")).size(), 5);

    EntityAssetLink link;
    link.entityType = QStringLiteral("well");
    link.entityId = QStringLiteral("well-000");
    link.assetId = QStringLiteral("ast-1");
    link.role = QStringLiteral("tops");
    QElapsedTimer timer;
    timer.start();
    QVERIFY(cat.addLink(link, &err)); // 一次 mutator = 一次全量 save()
    const qint64 elapsedMs = timer.elapsed();

    // 量测基线：实测 1–2ms（~330KB JSON + .bak 复制）。上限钉 100ms。
    qDebug() << "catalogSave(200 versions):" << elapsedMs << "ms (budget 100 ms)";
    QVERIFY2(elapsedMs < 100,
             qPrintable(QStringLiteral("catalog save took %1 ms > 100 ms budget").arg(elapsedMs)));
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
    qFatal("QgisRuntime::initialize failed");
  TestPerfBudget tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_perfbudget.moc"
