#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/lasparser.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"

#include <gdal.h>
#include <gdal_priv.h>
#include <qgsmaplayer.h>

// project_area 导入契约（docs/PROJECT_AREA_PLAN.md §2/§3）在真实服务栈上的验收：
// 实体解析/受管 RAW（SHA-256+只读）/外链/多井关联/双候选 unresolved/未决层位/
// D61 派生栅格与图层 CRS/LAS 解析。旧 §41.2 行为（data/<kind>/ + 矢量声明）
// 已被替换：.dat 不再交给 OGR，非栅格不再声明成矢量。
class TestImport : public QObject
{
  Q_OBJECT

  struct Stack
  {
    QgisProjectService projectSvc;
    std::unique_ptr<LayerManifest> manifest;
    std::unique_ptr<QgisLayerService> layerSvc;
    std::unique_ptr<PaleoProjectStore> store;
    std::unique_ptr<DataImportService> importSvc;
    QString metaPath;
  };

  static bool writeFile(const QString &path, const QByteArray &content)
  {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
      return false;
    f.write(content);
    return true;
  }

  static std::unique_ptr<Stack> makeStack(const QString &projectDir, QString *errOut = nullptr)
  {
    auto s = std::make_unique<Stack>();
    s->metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
    if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
    {
      if (errOut)
        *errOut = s->projectSvc.lastErrors().join(QLatin1Char(';'));
      return nullptr;
    }
    s->manifest = std::make_unique<LayerManifest>(s->metaPath);
    if (!s->manifest->open(errOut))
      return nullptr;
    s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
    s->store = std::make_unique<PaleoProjectStore>();
    s->store->setProjectPaths(QDir(projectDir).filePath(QStringLiteral("proj.qgz")),
                              QDir(projectDir).filePath(QStringLiteral("project.gpkg")),
                              s->metaPath);
    s->importSvc = std::make_unique<DataImportService>(s->layerSvc.get(), s->store.get());
    s->importSvc->setProjectDir(projectDir);
    return s;
  }

  static QString fixture(const QString &name)
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
  }

  // 摆进带语义的目录再导入（分类按路径段：井分层/时深/层位）。
  static QString stageFixture(const QTemporaryDir &tmp, const QString &dir,
                              const QString &name, const QString &asName = QString())
  {
    const QString d = tmp.filePath(dir);
    if (!QDir().mkpath(d))
      return QString();
    const QString dst = QDir(d).filePath(asName.isEmpty() ? name : asName);
    if (!QFile::copy(fixture(name), dst))
      return QString();
    return dst;
  }

  static QString sha256OfFile(const QString &path)
  {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(&f);
    return QString::fromLatin1(h.result().toHex());
  }

  // 在 service 打开前预置目录（预置井实体走独立 DataCatalog 落盘）。
  static bool seedCatalogWithSingleWell(const QString &projectDir)
  {
    DataCatalog pre;
    QString err;
    if (!pre.open(projectDir, &err))
      return false;
    CatalogEntity w;
    w.id = QStringLiteral("well-A1");
    w.entityType = QStringLiteral("well");
    w.name = QStringLiteral("A1");
    return pre.addEntity(w, &err);
  }

  static bool seedCatalogWithAliasWells(const QString &projectDir)
  {
    DataCatalog pre;
    QString err;
    if (!pre.open(projectDir, &err))
      return false;
    CatalogEntity w1;
    w1.id = QStringLiteral("well-X1");
    w1.entityType = QStringLiteral("well");
    w1.name = QStringLiteral("X1");
    w1.aliases = QStringList{QStringLiteral("dup")};
    if (!pre.addEntity(w1, &err))
      return false;
    CatalogEntity w2;
    w2.id = QStringLiteral("well-X2");
    w2.entityType = QStringLiteral("well");
    w2.name = QStringLiteral("X2");
    w2.aliases = QStringList{QStringLiteral("dup")};
    return pre.addEntity(w2, &err);
  }

private slots:
  void initTestCase()
  {
    QVERIFY(QgisRuntime::isInitialized());
  }

  // 井位 → 20 井实体（untransformed、surface 坐标），多井文件=每井一条关联。
  void wellHeadCreatesEntitiesAndLinks()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    QString err;
    const QString assetId = svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 20);
    const CatalogEntity a1 = cat->entityById(QStringLiteral("well-A1"));
    QCOMPARE(a1.name, QStringLiteral("A1"));
    QVERIFY(a1.hasSurface);
    QCOMPARE(a1.surfaceX, 5288.670);
    QCOMPARE(a1.surfaceY, 8219.940);
    QCOMPARE(a1.coordinateStatus, QStringLiteral("untransformed"));
    QVERIFY(a1.uwi.isEmpty());
    // 一份多井文件 → 20 条 well_head 关联（不拆文件）
    QCOMPARE(cat->linksForAsset(assetId).size(), 20);
    QCOMPARE(cat->linksForEntity(QStringLiteral("well-A1")).front().role,
             QStringLiteral("well_head"));
  }

  // A1.Las → 挂 well-A1；受管 RAW 落盘只读且 SHA-256 与源一致；无图层声明。
  void managedRawChecksumAndReadOnly()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    const QString assetId = svc.importProjectFile(fixture(QStringLiteral("A1.Las")), &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    const QVector<EntityAssetLink> links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.front().entityId, QStringLiteral("well-A1"));
    QCOMPARE(links.front().role, QStringLiteral("well_log"));
    QVERIFY(!links.front().unresolved);

    const CatalogVersion v = cat->currentVersion(assetId);
    QVERIFY(v.managed);
    QCOMPARE(v.stage, QStringLiteral("RAW"));
    const QString abs = svc.absolutePath(assetId);
    QVERIFY(abs.endsWith(QStringLiteral("artifacts/raw/") + assetId + QLatin1Char('/') + v.id +
                         QStringLiteral("/A1.Las")));
    QVERIFY(QFile::exists(abs));
    QCOMPARE(v.sha256, sha256OfFile(fixture(QStringLiteral("A1.Las"))));
    // 只读位：无任何写权限
    const QFile::Permissions perm = QFileInfo(abs).permissions();
    QCOMPARE(int(perm & (QFile::WriteOwner | QFile::WriteUser | QFile::WriteGroup | QFile::WriteOther)), 0);
    // .las 不再声明成矢量图层
    QVERIFY(stack->manifest->all().isEmpty());
    QCOMPARE(svc.assetSource(assetId), abs);
  }

  // DC.dat → 多井分层文件挂多条 tops 关联。
  void topsFileLinksEveryWell()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    const QString staged = stageFixture(tmp, QString::fromUtf8("井分层"), QStringLiteral("DC.dat"));
    QVERIFY(!staged.isEmpty());
    const QString assetId = svc.importProjectFile(staged, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    const QVector<EntityAssetLink> links = cat->linksForAsset(assetId);
    QVERIFY(links.size() >= 15); // 20 口井分层覆盖面
    bool sawA1 = false;
    for (const EntityAssetLink &l : links)
    {
      QCOMPARE(l.role, QStringLiteral("tops"));
      QVERIFY(!l.unresolved);
      if (l.entityId == QLatin1String("well-A1"))
        sawA1 = true;
    }
    QVERIFY(sawA1);
  }

  // 时深：井名列（# Well : A1）→ well-A1 + time_depth 关联。
  void timeDepthBindsByWellColumn()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    const QString staged = stageFixture(tmp, QString::fromUtf8("时深"), QStringLiteral("A1_TD.dat"));
    QVERIFY(!staged.isEmpty());
    const QString assetId = svc.importProjectFile(staged, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    const QVector<EntityAssetLink> links = svc.catalog()->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.front().entityId, QStringLiteral("well-A1"));
    QCOMPARE(links.front().role, QStringLiteral("time_depth"));
  }

  // 双候选 → 一条 unresolved 链接：实体 id 留空、备注写两个规范化井名，
  // 不新建井、不并井、不再生成 aux-unresolved 辅助实体（§3 修订）。
  void ambiguousWellNameYieldsUnresolvedLinks()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithAliasWells(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString lasPath = tmp.filePath(QStringLiteral("dup.Las"));
    QVERIFY(writeFile(lasPath, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. dup : WELL\n"
        "~Curve\nDEPT.M :\n~A DEPT\n100.0\n")));

    QString err;
    const QString assetId = svc.importProjectFile(lasPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    // 没有新建井
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    // 旧的 aux-unresolved 兜底已移除
    QVERIFY(cat->entities(QStringLiteral("auxiliary")).isEmpty());
    const QVector<EntityAssetLink> links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QVERIFY(links.front().unresolved);
    QVERIFY(links.front().entityId.isEmpty());
    QCOMPARE(links.front().entityType, QStringLiteral("well"));
    QCOMPARE(links.front().role, QStringLiteral("well_log"));
    // 备注记两个规范化井名（附实体 id 消歧）
    QVERIFY(links.front().note.contains(QStringLiteral("x1")));
    QVERIFY(links.front().note.contains(QStringLiteral("x2")));
    QVERIFY(links.front().note.contains(QStringLiteral("well-X1")));
    QVERIFY(links.front().note.contains(QStringLiteral("well-X2")));

    // 空实体 id + 备注都能过 catalog.json 往返
    DataCatalog reloaded;
    QVERIFY(reloaded.open(projectDir));
    const auto rl = reloaded.linksForAsset(assetId);
    QCOMPARE(rl.size(), 1);
    QVERIFY(rl.front().unresolved);
    QVERIFY(rl.front().entityId.isEmpty());
    QCOMPARE(rl.front().note, links.front().note);
  }

  // 零匹配（~W 名与文件名主名都落空）→ 资产保留、一条 unresolved 链接、
  // 实体 id 留空、备注记未匹配井名；不建井不建 aux（§3 修订）。
  void unmatchedWellLogLeavesUnresolvedLink()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString lasPath = tmp.filePath(QStringLiteral("Ghost.Las"));
    QVERIFY(writeFile(lasPath, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. Ghost9 : WELL\n"
        "~Curve\nDEPT.M :\n~A DEPT\n100.0\n")));

    QString err;
    const QString assetId = svc.importProjectFile(lasPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 1);     // 只有预置 A1
    QVERIFY(cat->entities(QStringLiteral("auxiliary")).isEmpty()); // 无 aux-unresolved
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QVERIFY(links.front().unresolved);
    QVERIFY(links.front().entityId.isEmpty());
    QCOMPARE(links.front().entityType, QStringLiteral("well"));
    QVERIFY(links.front().note.contains(QStringLiteral("ghost9"))); // ~W 名
    QVERIFY(links.front().note.contains(QStringLiteral("ghost")));  // 文件名主名

    DataCatalog reloaded;
    QVERIFY(reloaded.open(projectDir));
    const auto rl = reloaded.linksForAsset(assetId);
    QCOMPARE(rl.size(), 1);
    QVERIFY(rl.front().unresolved);
    QVERIFY(rl.front().entityId.isEmpty());
    QCOMPARE(rl.front().note, links.front().note);
  }

  // 多井分层文件：能解析的井名挂接；未匹配井名也各留一条 unresolved 链接
  //（实体 id 留空、备注记名），不再静默丢掉或挂 aux（§3 修订）。
  void topsFileLeavesUnmatchedNamesUnresolved()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString dir = tmp.filePath(QString::fromUtf8("井分层"));
    QVERIFY(QDir().mkpath(dir));
    const QString topsPath = QDir(dir).filePath(QStringLiteral("tops_mixed.dat"));
    QVERIFY(writeFile(topsPath, QByteArrayLiteral(
        "#WellTops File From SMI\n"
        "#WellName    Name         MD\n"
        "A1           D61          2148.000\n"
        "Ghost9       D61          2150.000\n")));

    QString err;
    const QString assetId = svc.importProjectFile(topsPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 1);
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 2);
    bool sawResolved = false, sawUnresolved = false;
    for (const EntityAssetLink &l : links)
    {
      QCOMPARE(l.role, QStringLiteral("tops"));
      QCOMPARE(l.entityType, QStringLiteral("well"));
      if (!l.unresolved)
      {
        QCOMPARE(l.entityId, QStringLiteral("well-A1"));
        sawResolved = true;
      }
      else
      {
        QVERIFY(l.entityId.isEmpty());
        QVERIFY(l.note.contains(QStringLiteral("ghost9")));
        sawUnresolved = true;
      }
    }
    QVERIFY(sawResolved && sawUnresolved);
  }

  // 井口行恰好匹配一口已有井 → 挂 well_head，不另建井（§3）。
  void wellHeadRowMatchingExistingWellDoesNotDuplicate()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString dir = tmp.filePath(QString::fromUtf8("井位"));
    QVERIFY(QDir().mkpath(dir));
    const QString whPath = QDir(dir).filePath(QStringLiteral("heads.dat"));
    QVERIFY(writeFile(whPath, QByteArrayLiteral(
        "#WellHead File From SMI\n"
        "#Name      X     Y     KB    TotalDepth\n"
        "A-1        100.0 200.0 0.0   2000.0\n"   // 规范化 a1 → 命中已有 well-A1
        "B9         300.0 400.0 0.0   2100.0\n"))); // 无匹配 → 新建

    QString err;
    const QString assetId = svc.importProjectFile(whPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2); // A1 + 新建 B9
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 2);
    QStringList linked;
    for (const EntityAssetLink &l : links)
    {
      QCOMPARE(l.role, QStringLiteral("well_head"));
      QVERIFY(!l.unresolved);
      linked.append(l.entityId);
    }
    linked.sort();
    QCOMPARE(linked, QStringList({QStringLiteral("well-A1"), QStringLiteral("well-B9")}));
    QCOMPARE(cat->entityById(QStringLiteral("well-A1")).name, QStringLiteral("A1"));
  }

  // 井口文件内同一规范化井名出现两行 → 两行都 unresolved，不建井（§3）。
  void wellHeadDuplicateNormalizedNameStaysUnresolved()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString dir = tmp.filePath(QString::fromUtf8("井位"));
    QVERIFY(QDir().mkpath(dir));
    const QString whPath = QDir(dir).filePath(QStringLiteral("heads.dat"));
    QVERIFY(writeFile(whPath, QByteArrayLiteral(
        "#WellHead File From SMI\n"
        "#Name      X     Y     KB    TotalDepth\n"
        "X9         100.0 200.0 0.0   2000.0\n"
        "X-9        110.0 210.0 0.0   2000.0\n"))); // 与 X9 规范化同名

    QString err;
    const QString assetId = svc.importProjectFile(whPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    QVERIFY(cat->entities(QStringLiteral("well")).isEmpty());
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 2);
    for (const EntityAssetLink &l : links)
    {
      QCOMPARE(l.role, QStringLiteral("well_head"));
      QVERIFY(l.unresolved);
      QVERIFY(l.entityId.isEmpty());
      QVERIFY(l.note.contains(QStringLiteral("x9")));
    }
  }

  // 井口行同时匹配两口已有井 → 标 unresolved，实体 id 留空，不新建不合并（§3）。
  void wellHeadRowMatchingTwoWellsStaysUnresolved()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithAliasWells(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString dir = tmp.filePath(QString::fromUtf8("井位"));
    QVERIFY(QDir().mkpath(dir));
    const QString whPath = QDir(dir).filePath(QStringLiteral("heads.dat"));
    QVERIFY(writeFile(whPath, QByteArrayLiteral(
        "#WellHead File From SMI\n"
        "#Name      X     Y     KB    TotalDepth\n"
        "dup        100.0 200.0 0.0   2000.0\n")));

    QString err;
    const QString assetId = svc.importProjectFile(whPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QVERIFY(links.front().unresolved);
    QVERIFY(links.front().entityId.isEmpty());
    QVERIFY(links.front().note.contains(QStringLiteral("x1")));
    QVERIFY(links.front().note.contains(QStringLiteral("x2")));
  }

  // D61 → RAW + DERIVED 栅格（父版本指向 RAW）+ 图层声明；authid 非 4326；
  // A1 落点压在栅格非空像元上（半像元容差）。
  void horizonDerivedRasterAndCrs()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    const QString staged = stageFixture(tmp, QString::fromUtf8("层位"),
                                        QStringLiteral("D61_sample.dat"), QStringLiteral("D61.dat"));
    QVERIFY(!staged.isEmpty());
    const QString assetId = svc.importProjectFile(staged, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    const CatalogEntity sb = cat->entityById(QStringLiteral("sb-D61"));
    QCOMPARE(sb.entityType, QStringLiteral("sequence_boundary"));
    QVERIFY(!sb.extra.contains(QStringLiteral("pending")));

    const QVector<CatalogVersion> versions = cat->versionsForAsset(assetId);
    QCOMPARE(versions.size(), 2);
    const CatalogVersion raw = cat->currentVersion(assetId);
    QCOMPARE(raw.stage, QStringLiteral("DERIVED"));
    QCOMPARE(raw.versionNumber, 2);
    QCOMPARE(raw.parentVersionIds.size(), 1);
    QVERIFY(raw.extra.contains(QStringLiteral("collisions")));

    // 图层清单只登记要画的结果
    const QVector<LayerDeclaration> decls = stack->manifest->all();
    QCOMPARE(decls.size(), 1);
    QCOMPARE(decls.at(0).layerId, QStringLiteral("horizon.D61"));
    QCOMPARE(decls.at(0).horizon, QStringLiteral("D61"));
    QCOMPARE(decls.at(0).type, QStringLiteral("raster"));

    // 实例化后图层 CRS 不是 EPSG:4326
    QgsMapLayer *layer = stack->layerSvc->instantiate(QStringLiteral("horizon.D61"), &err);
    QVERIFY2(layer != nullptr, qPrintable(err));
    const QString authid = layer->crs().authid();
    QVERIFY2(authid != QLatin1String("EPSG:4326"),
             qPrintable(QStringLiteral("authid=") + authid));

    // A1 (5288.67, 8219.94) 压在 D61 栅格非空像元（半像元容差 → 相邻格都查）。
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen(decls.at(0).source.toUtf8().constData(), GA_ReadOnly);
    QVERIFY2(ds, "open derived raster");
    double gt[6] = {0, 0, 0, 0, 0, 0};
    GDALGetGeoTransform(ds, gt);
    const double px = (5288.67 - gt[0]) / gt[1];
    const double py = (8219.94 - gt[3]) / gt[5];
    int hasNd = 0;
    const double nd = GDALGetRasterNoDataValue(GDALGetRasterBand(ds, 1), &hasNd);
    float v1 = 0, v2 = 0, v3 = 0, v4 = 0;
    const int pxl = qBound(0, int(std::floor(px)), GDALGetRasterXSize(ds) - 1);
    const int pyl = qBound(0, int(std::floor(py)), GDALGetRasterYSize(ds) - 1);
    QVERIFY(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, pxl, pyl, 1, 1, &v1, 1, 1, GDT_Float32, 0, 0) == CE_None);
    QVERIFY(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, qMin(pxl + 1, GDALGetRasterXSize(ds) - 1), pyl, 1, 1,
                 &v2, 1, 1, GDT_Float32, 0, 0) == CE_None);
    QVERIFY(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, pxl, qMin(pyl + 1, GDALGetRasterYSize(ds) - 1), 1, 1,
                 &v3, 1, 1, GDT_Float32, 0, 0) == CE_None);
    QVERIFY(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, qMin(pxl + 1, GDALGetRasterXSize(ds) - 1),
                 qMin(pyl + 1, GDALGetRasterYSize(ds) - 1), 1, 1, &v4, 1, 1, GDT_Float32, 0, 0) == CE_None);
    GDALClose(ds);
    QVERIFY2(hasNd && (v1 != nd || v2 != nd || v3 != nd || v4 != nd),
             "A1 must sit on a filled D61 cell (half-pixel tolerance)");
  }

  // 文件名不在 8 界面集合 → 未决层位实体 + unresolved 关联，不进图层清单。
  void unknownHorizonStaysPending()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    const QString src = stageFixture(tmp, QString::fromUtf8("层位"),
                                      QStringLiteral("D61_sample.dat"), QStringLiteral("ZZ9.dat"));
    QVERIFY(!src.isEmpty());
    QString err;
    const QString assetId = svc.importProjectFile(src, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    const CatalogEntity sb = cat->entityById(QStringLiteral("sb-ZZ9"));
    QCOMPARE(sb.entityType, QStringLiteral("sequence_boundary"));
    QVERIFY(sb.extra.value(QStringLiteral("pending")).toBool());
    const QVector<EntityAssetLink> links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QVERIFY(links.front().unresolved);
    QCOMPARE(stack->manifest->all().size(), 0); // 不进编图 chip/地图
  }

  // 地震外链：版本 managed=false + 源绝对路径；survey 几何冻结；源缺失可检测。
  void seismicExternalLinkAndGeometry()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    const QString sgy = tmp.filePath(QStringLiteral("200P_mini.sgy"));
    QVERIFY(QFile::copy(fixture(QStringLiteral("mini_seismic.sgy")), sgy));
    const QString assetId = svc.importProjectFile(sgy, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    const CatalogVersion v = cat->currentVersion(assetId);
    QVERIFY(!v.managed);
    QVERIFY(QFileInfo(v.path).isAbsolute());
    QVERIFY(v.sha256.isEmpty());
    QCOMPARE(svc.absolutePath(assetId), v.path);

    const CatalogEntity survey = cat->entityById(QStringLiteral("survey-200P_mini"));
    QCOMPARE(survey.entityType, QStringLiteral("seismic_survey"));
    QCOMPARE(survey.inlineMin, 1000.0);
    QCOMPARE(survey.inlineMax, 1002.0);
    QCOMPARE(survey.sampleIntervalUs, 2000.0);
    QCOMPARE(cat->linksForAsset(assetId).front().role, QStringLiteral("seismic_volume"));

    // 外链缺失检测（预览「找不到源文件」的判定基础）
    QVERIFY(QFile::exists(v.path));
    QFile::remove(v.path);
    QVERIFY(!QFile::exists(v.path));
  }

  // 阶段 D：GeoJSON 图例字典（相/亚相/微相 distinct 值）落辅助实体。
  void geojsonLegendDictionaryCollected()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    const QString assetId = svc.importProjectFile(fixture(QStringLiteral("facies.geojson")), &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.front().role, QStringLiteral("reference"));
    const CatalogEntity aux = cat->entityById(links.front().entityId);
    QCOMPARE(aux.entityType, QStringLiteral("auxiliary"));
    QVERIFY(!aux.extra.value(QStringLiteral("georeferenced")).toBool());
    const QVariantMap legend = aux.extra.value(QStringLiteral("legend")).toMap();
    const QStringList facies = legend.value(QString::fromUtf8("相")).toStringList();
    const QStringList subFacies = legend.value(QString::fromUtf8("亚相")).toStringList();
    QCOMPARE(facies.size(), 2);
    QVERIFY(facies.contains(QString::fromUtf8("三角洲前缘")));
    QVERIFY(facies.contains(QString::fromUtf8("滨浅湖")));
    QCOMPARE(subFacies.size(), 2);
    QVERIFY(subFacies.contains(QString::fromUtf8("河口坝")));
    QVERIFY(subFacies.contains(QString::fromUtf8("滩坝")));
    // 未配准：不生成地图图层
    QVERIFY(stack->manifest->all().isEmpty());
  }

  // 阶段 D：参考资料目录 / HZ28-6-1 命名的 XML 固定辅助参考，不按内容挂井。
  void hz28XmlStaysAuxiliaryReference()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    // 预置井 A1，验证不会并进去
    QVERIFY(seedCatalogWithSingleWell(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    const QString refDir = tmp.filePath(QString::fromUtf8("参考资料"));
    QVERIFY(QDir().mkpath(refDir));
    const QString xmlPath = QDir(refDir).filePath(
        QStringLiteral("HZ28-6-1井综合柱状图-2021-沉积-地化室-未钻遇烃源岩层-测井-惠州勘探室.xml"));
    // 内容故意写成测井 XML——也不允许按内容挂井
    QVERIFY(writeFile(xmlPath, QByteArrayLiteral(
        "<logs><log><logcurveinfo/><logdata>1 2</logdata></log></logs>")));

    QString err;
    const QString assetId = svc.importProjectFile(xmlPath, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
    DataCatalog *cat = svc.catalog();
    // 井实体只有预置的一口（没有 HZ28 井、没有挂到 A1）
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 1);
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QCOMPARE(links.front().entityType, QStringLiteral("auxiliary"));
    QCOMPARE(links.front().role, QStringLiteral("reference"));
    QVERIFY(!links.front().unresolved);
    QVERIFY(cat->linksForEntity(QStringLiteral("well-A1")).isEmpty());
  }

  // 缺失源 → 失败信号，无登记。
  void importMissingSourceFails()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QSignalSpy failedSpy(&svc, &DataImportService::importFailed);
    QSignalSpy importedSpy(&svc, &DataImportService::imported);

    QString err;
    const QString assetId = svc.importFile(QStringLiteral("wells"),
                                           QStringLiteral("/nonexistent/ghost.las"), &err);
    QVERIFY(assetId.isEmpty());
    QVERIFY(!err.isEmpty());
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy.at(0).at(1).toString(), QStringLiteral("/nonexistent/ghost.las"));
    QCOMPARE(importedSpy.count(), 0);
    QVERIFY(svc.assets().isEmpty());
    QVERIFY(stack->manifest->all().isEmpty());
  }

  // LAS: ~V/~W/~C/~A parsed; 3 curves × 4 rows; NULL token → NaN.
  void lasParsesCurvesAndNulls()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString lasPath = tmp.filePath(QStringLiteral("w01.las"));
    QVERIFY(writeFile(lasPath, QByteArrayLiteral(
        "~Version Information\n"
        " VERS.                  2.0 :   CWLS LOG ASCII STANDARD -VERSION 2.0\n"
        " WRAP.                  NO  :   ONE LINE PER DEPTH STEP\n"
        "~Well Information Block\n"
        "#MNEM.UNIT       DATA            INFORMATION\n"
        " STRT.M              1670.0000 :   START DEPTH\n"
        " STOP.M              1669.5000 :   STOP DEPTH\n"
        " STEP.M               -0.1250 :   STEP\n"
        " NULL.               -999.2500 :   NULL VALUE\n"
        " WELL.                  W-01   :   WELL NAME\n"
        "~Curve Information Block\n"
        " DEPT.M                    :   1  DEPTH\n"
        " GR  .API                  :   2  GAMMA RAY\n"
        " DT  .US/F                 :   3  SONIC DELTA-T\n"
        "~A  DEPT       GR       DT\n"
        "1670.000  82.10  460.00\n"
        "1669.875  81.50  455.20\n"
        "1669.750  -999.25 450.90\n"
        "1669.625  79.80  446.10\n")));

    QStringList names;
    QList<LasCurve> curves;
    QString err;
    QVERIFY2(LasParser::parse(lasPath, names, curves, &err), qPrintable(err));

    QCOMPARE(names, QStringList({QStringLiteral("DEPT"), QStringLiteral("GR"), QStringLiteral("DT")}));
    QCOMPARE(curves.size(), 3);
    QCOMPARE(curves.at(0).name, QStringLiteral("DEPT"));
    QCOMPARE(curves.at(0).unit, QStringLiteral("M"));
    QCOMPARE(curves.at(1).name, QStringLiteral("GR"));
    QCOMPARE(curves.at(1).unit, QStringLiteral("API"));
    QCOMPARE(curves.at(2).descr, QStringLiteral("3  SONIC DELTA-T"));

    for (const LasCurve &c : curves)
      QCOMPARE(c.values.size(), 4);

    QCOMPARE(curves.at(0).values.at(0), 1670.0);
    QCOMPARE(curves.at(0).values.at(3), 1669.625);
    QCOMPARE(curves.at(1).values.at(0), 82.10);
    QVERIFY(qIsNaN(curves.at(1).values.at(2)));  // -999.25 NULL → NaN
    QCOMPARE(curves.at(1).values.at(3), 79.80);
    QCOMPARE(curves.at(2).values.at(2), 450.90);
    QVERIFY(!qIsNaN(curves.at(2).values.at(2)));
  }

  // WRAP YES → hard error; missing file → error.
  void lasFailureModes()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString wrapPath = tmp.filePath(QStringLiteral("wrap.las"));
    QVERIFY(writeFile(wrapPath, QByteArrayLiteral(
        "~Version Information\n"
        " VERS.                  2.0 :   CWLS LOG ASCII STANDARD -VERSION 2.0\n"
        " WRAP.                  YES :   MULTIPLE LINES PER DEPTH STEP\n"
        "~Curve Information\n"
        " DEPT.M : DEPTH\n"
        "~A DEPT\n"
        "1670.0\n")));

    QStringList names;
    QList<LasCurve> curves;
    QString err;
    QVERIFY(!LasParser::parse(wrapPath, names, curves, &err));
    QVERIFY(!err.isEmpty());

    QVERIFY(!LasParser::parse(tmp.filePath(QStringLiteral("ghost.las")), names, curves, &err));
    QVERIFY(!err.isEmpty());
  }
};

int main(int argc, char *argv[])
{
  // Offscreen QGIS bootstrap through the runtime that owns init order.
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestImport tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_import.moc"
