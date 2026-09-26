#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include <algorithm>
#if defined(Q_OS_UNIX)
#include <sys/stat.h> // mkfifo（文件夹导入的「非普通文件」行）
#endif

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/lasparser.h"
#include "../src/io/projectclassifier.h"
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

  // D12：歧义改用同名两井构造（uwi/aliases 字段已剥离）。
  static bool seedCatalogWithAliasWells(const QString &projectDir)
  {
    DataCatalog pre;
    QString err;
    if (!pre.open(projectDir, &err))
      return false;
    CatalogEntity w1;
    w1.id = QStringLiteral("well-X1");
    w1.entityType = QStringLiteral("well");
    w1.name = QStringLiteral("dup");
    if (!pre.addEntity(w1, &err))
      return false;
    CatalogEntity w2;
    w2.id = QStringLiteral("well-X2");
    w2.entityType = QStringLiteral("well");
    w2.name = QStringLiteral("dup");
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
    // 备注记两个候选（D12：同名井，靠实体 id 消歧）
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
    // D12：同名双候选——备注靠实体 id 消歧
    QVERIFY(links.front().note.contains(QStringLiteral("well-X1")));
    QVERIFY(links.front().note.contains(QStringLiteral("well-X2")));
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
    // §3：外链也留入库时 SHA-256（流式算的源文件摘要），打开时照它校验。
    QCOMPARE(v.sha256, sha256OfFile(sgy));
    QVERIFY(cat->verifyExternalVersionSha(v));
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

  // §3 外链篡改：源文件字节变了 → 打开时 SHA-256 校验失败、报固定文案，不解码。
  void externalShaMismatchBlocksOpen()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    const QString sgy = tmp.filePath(QStringLiteral("vol.sgy"));
    QVERIFY(QFile::copy(fixture(QStringLiteral("mini_seismic.sgy")), sgy));
    const QString assetId = svc.importProjectFile(sgy, &err);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));

    DataCatalog *cat = svc.catalog();
    const CatalogVersion v = cat->currentVersion(assetId);
    QVERIFY(!v.managed);
    QVERIFY(cat->verifyExternalVersionSha(v)); // 未动过 → 校验通过

    // 尾部加一个字节 → 摘要变了
    QFile f(sgy);
    QVERIFY(f.open(QIODevice::Append));
    QCOMPARE(f.write("X"), 1);
    f.close();
    QString verr;
    QVERIFY(!cat->verifyExternalVersionSha(v, &verr));
    QCOMPARE(verr, QStringLiteral("源文件与入库时的 SHA-256 不一致"));
  }

  // §3 dedup：同一 SHA-256 再导入 → 不新建资产/版本/主关联，报「字节已在库」。
  void sameShaReimportDoesNotDuplicate()
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
    const int assetCount = cat->assets().size();
    int versionCount = 0;
    for (const CatalogAsset &a : cat->assets())
      versionCount += cat->versionsForAsset(a.id).size();

    const DataImportService::ImportResult res =
        svc.importProjectFileEx(fixture(QStringLiteral("A1.Las")), &err);
    QVERIFY2(res.outcome == DataImportService::ImportOutcome::AlreadyStored,
             qPrintable(res.message));
    QCOMPARE(res.assetId, assetId); // 指回已存在资产
    QVERIFY(!res.linkAttached);
    QVERIFY(res.message.contains(QStringLiteral("字节已在库")));
    QVERIFY(res.message.contains(QStringLiteral("没有新的关联")));

    QCOMPARE(cat->assets().size(), assetCount); // 没有新资产
    int versionCount2 = 0;
    for (const CatalogAsset &a : cat->assets())
      versionCount2 += cat->versionsForAsset(a.id).size();
    QCOMPARE(versionCount2, versionCount); // 没有新版本

    // A1 的 well_log 仍只有一条主关联——没有第二条。
    int primaryLogs = 0, totalLogs = 0;
    for (const EntityAssetLink &l : cat->linksForEntity(QStringLiteral("well-A1")))
      if (l.role == QLatin1String("well_log"))
      {
        ++totalLogs;
        if (l.isPrimary)
          ++primaryLogs;
      }
    QCOMPARE(totalLogs, 1);
    QCOMPARE(primaryLogs, 1);
  }

  // §3 dedup 补挂：入库时未决的链接，在该井出现后重导同一文件 → 补一条主关联。
  void reimportAttachesNowResolvableLink()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir)); // 只有 A1
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
    QVERIFY(cat->linksForAsset(assetId).front().unresolved);
    const int unresolvedBefore = cat->links().size();

    // Ghost9 井后出现 → 再导同一文件：dedup 命中且补上关联。
    CatalogEntity w;
    w.id = QStringLiteral("well-Ghost9");
    w.entityType = QStringLiteral("well");
    w.name = QStringLiteral("Ghost9");
    QVERIFY(cat->addEntity(w, &err));

    const DataImportService::ImportResult res = svc.importProjectFileEx(lasPath, &err);
    QVERIFY(res.outcome == DataImportService::ImportOutcome::AlreadyStored);
    QCOMPARE(res.assetId, assetId);
    QVERIFY(res.linkAttached);
    QVERIFY(res.message.contains(QStringLiteral("字节已在库")));
    QVERIFY(res.message.contains(QStringLiteral("已补上关联")));

    QCOMPARE(cat->links().size(), unresolvedBefore); // 没有新增链接
    const auto links = cat->linksForAsset(assetId);
    QCOMPARE(links.size(), 1);
    QVERIFY(!links.front().unresolved);
    QCOMPARE(links.front().entityId, QStringLiteral("well-Ghost9"));
    QVERIFY(links.front().isPrimary);
    QVERIFY(links.front().note.isEmpty());
    // catalog.json 往返后仍在
    DataCatalog reloaded;
    QVERIFY(reloaded.open(projectDir, &err));
    QCOMPARE(reloaded.linksForAsset(assetId).front().entityId, QStringLiteral("well-Ghost9"));
  }

  // §3：新 SHA-256 追加不可变版本/新资产——同井同角色的旧主关联降级，
  // 同一角色只留一条主关联。
  void newShaVersionDemotesPreviousPrimaryLink()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    QString err;
    QVERIFY(!svc.importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err).isEmpty());
    const QString oldAsset = svc.importProjectFile(fixture(QStringLiteral("A1.Las")), &err);
    QVERIFY2(!oldAsset.isEmpty(), qPrintable(err));

    // 同井新字节 → 新资产 + 新主关联
    const QString v2 = tmp.filePath(QStringLiteral("A1_v2.las"));
    QVERIFY(QFile::copy(fixture(QStringLiteral("A1.Las")), v2));
    {
      QFile f(v2);
      QVERIFY(f.open(QIODevice::Append));
      QCOMPARE(f.write("\n"), 1);
    }
    const QString newAsset = svc.importProjectFile(v2, &err);
    QVERIFY2(!newAsset.isEmpty(), qPrintable(err));
    QVERIFY(newAsset != oldAsset);

    DataCatalog *cat = svc.catalog();
    int primaryLogs = 0;
    QString primaryAsset;
    for (const EntityAssetLink &l : cat->linksForEntity(QStringLiteral("well-A1")))
    {
      if (l.role != QLatin1String("well_log"))
        continue;
      if (l.isPrimary)
      {
        ++primaryLogs;
        primaryAsset = l.assetId;
      }
    }
    QCOMPARE(primaryLogs, 1);              // 同一角色只留一条主关联
    QCOMPARE(primaryAsset, newAsset);      // 且是新的那份
  }

  // §3 路径卫生：文件名含换行/控制字符或 ".." → 这一行如实失败，不入库。
  void unsafeFileNameRejected()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    // 换行
    const QString nl = tmp.filePath(QStringLiteral("bad\nname.las"));
    QVERIFY(writeFile(nl, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A9 : WELL\n"
        "~Curve\nDEPT.M :\n~A DEPT\n1\n")));
    QString err;
    QVERIFY(svc.importProjectFile(nl, &err).isEmpty());
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains(QStringLiteral("路径段")));

    // 控制字符（Tab）
    const QString tab = tmp.filePath(QStringLiteral("bad\tname.las"));
    QVERIFY(writeFile(tab, QByteArrayLiteral("x")));
    QVERIFY(svc.importProjectFile(tab, &err).isEmpty());
    QVERIFY(!err.isEmpty());

    // ".."
    const QString dd = tmp.filePath(QStringLiteral("..evil.las"));
    QVERIFY(writeFile(dd, QByteArrayLiteral("x")));
    QVERIFY(svc.importProjectFile(dd, &err).isEmpty());
    QVERIFY(!err.isEmpty());

    QVERIFY(svc.assets().isEmpty()); // 一个都没登记
    QVERIFY(stack->manifest->all().isEmpty());
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

  // 「导入工区文件夹」（§3 + autoplan-eng/dx）：递归收普通文件（分类依赖
  // 井位/井分层/时深 路径段）；井口行先处理——LAS 文件名排在井口前面也照
  // 常挂到 A1；逃逸符号链接与非普通文件标 Skipped；单行失败不中断；
  // dedup 行报「字节已在库」。
  void folderImportWellHeadsFirstAndIsolatesFailures()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = DataImportService::FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井位"))));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井分层"))));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("时深/TD"))));

    // 文件名排在最前的 LAS：证明井口先行是两阶段而非按名序。
    const QString lasPath = QDir(root).filePath(QStringLiteral("0A1.Las"));
    QVERIFY(writeFile(lasPath, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\n~A DEPT\n100.0\n")));
    // 幽灵 LAS → 未决行（~W 名与文件名主名都落空），不建井。
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("ghost.las")), QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. Ghost9 : WELL\n"
        "~Curve\nDEPT.M :\n~A DEPT\n100.0\n")));
    // 同井第二份 LAS：它的主关联把 0A1.Las 的降级——降级≠未决，行仍算入库。
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("zzA1b.las")), QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A1 : WELL\n"
        "~Curve\nDEPT.M :\n~A DEPT\n101.0\n")));
    // 井位（带 BOM 同真文件）：heads.dat 建 A1+B2；empty.dat 只有注释 → 行失败。
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/heads.dat")),
        QByteArrayLiteral("\xEF\xBB\xBF#WellHead File From SMI\n"
                          "#Name      X     Y     KB    TotalDepth\n"
                          "A1           1.0   2.0   0.0   2000.0\n"
                          "B2           3.0   4.0   0.0   2100.0\n")));
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/empty.dat")),
                      QByteArrayLiteral("#WellHead File From SMI\n# no rows\n")));
    // tops：A1 已决 + Ghost9 未决 → 单行入库（主关联已写）+ 未决备注。
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井分层/tops.dat")),
        QByteArrayLiteral("#WellTops File From SMI\n#WellName  Name  MD\n"
                          "A1       D61   2148.0\n"
                          "Ghost9   D61   2150.0\n")));
    // 时深在 TD 二级子目录——证明递归下钻而不止一层。
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("时深/TD/a1.dat")),
        QByteArrayLiteral("#TimeDepth File From SMI\n# Well : A1\n"
                          "#TIME  TVDSS    TVD      MD\n"
                          "380.0  -252.6   252.6   252.6\n"
                          "382.0  -260.7   260.7   260.7\n")));

    // 逃逸符号链接 → Skipped；根内符号链接 → 正常走（dedup 命中）。
    const QString outside = tmp.filePath(QStringLiteral("outside.las"));
    QVERIFY(writeFile(outside, QByteArrayLiteral("x")));
    QVERIFY(QFile::link(outside, QDir(root).filePath(QStringLiteral("escape.las"))));
    QVERIFY(QFile::link(lasPath, QDir(root).filePath(QStringLiteral("mirror.Las"))));

    // 非普通文件（fifo）→ Skipped 行。
    bool madeFifo = false;
#if defined(Q_OS_UNIX)
    const QString fifoPath = QDir(root).filePath(QStringLiteral("pipe.sock"));
    madeFifo = ::mkfifo(QFile::encodeName(fifoPath).constData(), 0600) == 0;
#endif

    QString err;
    const QVector<DataImportService::FolderRowResult> rows = svc.importFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 9 + (madeFifo ? 1 : 0));

    const auto expectPath = [&rows](int i, const QString &suffix) {
      QVERIFY2(rows.at(i).path.endsWith(suffix),
               qPrintable(QStringLiteral("row %1 = %2, want *%3")
                              .arg(i).arg(rows.at(i).path, suffix)));
    };

    // 阶段 1：两个 well_head 行在最前（井位 内按路径排序：empty < heads）。
    expectPath(0, QString::fromUtf8("井位/empty.dat"));
    QCOMPARE(rows.at(0).classifiedType, QStringLiteral("well_head"));
    QCOMPARE(rows.at(0).outcome, Outcome::Failed);
    QVERIFY(rows.at(0).message.contains(QStringLiteral("no well head rows")));
    QVERIFY(rows.at(0).entityName.isEmpty());

    expectPath(1, QString::fromUtf8("井位/heads.dat"));
    QCOMPARE(rows.at(1).classifiedType, QStringLiteral("well_head"));
    QCOMPARE(rows.at(1).outcome, Outcome::Imported);
    QVERIFY(rows.at(1).entityName.contains(QStringLiteral("A1")));
    QVERIFY(rows.at(1).entityName.contains(QStringLiteral("B2")));

    // 阶段 2（路径排序）：0A1.Las、ghost.las、mirror.Las、井分层、时深/TD。
    expectPath(2, QStringLiteral("0A1.Las"));
    QCOMPARE(rows.at(2).classifiedType, QStringLiteral("well_log"));
    QCOMPARE(rows.at(2).outcome, Outcome::Imported); // 井口已先行 → 照常挂上
    QCOMPARE(rows.at(2).entityName, QStringLiteral("A1"));

    expectPath(3, QStringLiteral("ghost.las"));
    QCOMPARE(rows.at(3).outcome, Outcome::Unresolved);
    QVERIFY(rows.at(3).entityName.isEmpty());
    QVERIFY(rows.at(3).message.contains(QStringLiteral("ghost9")));

    expectPath(4, QStringLiteral("mirror.Las")); // 根内符号链接 → 处理且 dedup
    QCOMPARE(rows.at(4).outcome, Outcome::Imported);
    QVERIFY(rows.at(4).message.contains(QStringLiteral("字节已在库")));

    expectPath(5, QStringLiteral("zzA1b.las")); // 同井第二份 LAS → 入库
    QCOMPARE(rows.at(5).outcome, Outcome::Imported);
    QCOMPARE(rows.at(5).entityName, QStringLiteral("A1"));

    expectPath(6, QString::fromUtf8("井分层/tops.dat"));
    QCOMPARE(rows.at(6).classifiedType, QStringLiteral("well_stratification"));
    QCOMPARE(rows.at(6).outcome, Outcome::Imported); // 有主关联写出 → 入库
    QVERIFY(rows.at(6).entityName.contains(QStringLiteral("A1")));
    QVERIFY(rows.at(6).message.contains(QStringLiteral("ghost9"))); // 附未决备注

    expectPath(7, QString::fromUtf8("时深/TD/a1.dat"));
    QCOMPARE(rows.at(7).classifiedType, QStringLiteral("time_depth"));
    QCOMPARE(rows.at(7).outcome, Outcome::Imported);
    QCOMPARE(rows.at(7).entityName, QStringLiteral("A1"));

    // Skipped 行缀在最后（按路径排序）：escape.las 先于 pipe.sock。
    expectPath(8, QStringLiteral("escape.las"));
    QCOMPARE(rows.at(8).outcome, Outcome::Skipped);
    QVERIFY(rows.at(8).message.contains(QStringLiteral("符号链接")));
    QVERIFY(rows.at(8).message.contains(QStringLiteral("之外")));
    if (madeFifo)
    {
      expectPath(9, QStringLiteral("pipe.sock"));
      QCOMPARE(rows.at(9).outcome, Outcome::Skipped);
      QVERIFY(rows.at(9).message.contains(QStringLiteral("普通文件")));
    }

    // 顺序不变式：所有 well_head 行都排在所有非井口行前面。
    int lastHead = -1, firstOther = rows.size();
    for (int i = 0; i < rows.size(); ++i)
    {
      if (rows.at(i).outcome == Outcome::Skipped)
        continue;
      if (rows.at(i).classifiedType == QLatin1String("well_head"))
        lastHead = i;
      else if (firstOther == rows.size())
        firstOther = i;
    }
    QVERIFY(lastHead >= 0 && lastHead < firstOther);

    // catalog：A1+B2 两口井（ghost 不建井）；A1 恰好四条主关联（autoplan
    // 「LAS 排前仍得四条主关联」）；空井口失败行不留链接；0A1.Las 的
    // well_log 关联被 zzA1b.las 降级——同一角色只留一条主关联。
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 2);
    QVERIFY(cat->hasEntity(QStringLiteral("well-B2")));
    QStringList roles;
    int logLinks = 0;
    for (const EntityAssetLink &l : cat->linksForEntity(QStringLiteral("well-A1")))
    {
      if (l.isPrimary && !l.unresolved)
        roles.append(l.role);
      if (l.role == QLatin1String("well_log"))
        ++logLinks;
    }
    QCOMPARE(logLinks, 2); // 两条 well_log 链接，一条已降级
    std::sort(roles.begin(), roles.end());
    QCOMPARE(roles, QStringList({QStringLiteral("time_depth"), QStringLiteral("tops"),
                                 QStringLiteral("well_head"), QStringLiteral("well_log")}));
    QCOMPARE(cat->assets().size(), 7); // ghost/empty 也各占一份资产；mirror/escape/fifo 无
    QCOMPARE(cat->links().size(), 8);  // 2 井口 + 2 tops + 2 LAS + 1 ghost + 1 TD
  }

  // 文件夹入口的目录级失败：不存在/是文件/空目录/选中了工程目录自身。
  void folderImportRejectsBadRoots()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    QString err;
    QVERIFY(svc.importFolder(tmp.filePath(QStringLiteral("nonexistent")), &err).isEmpty());
    QVERIFY(!err.isEmpty());

    const QString aFile = tmp.filePath(QStringLiteral("a.las"));
    QVERIFY(writeFile(aFile, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. A9 : WELL\n"
        "~Curve\nDEPT.M :\n~A DEPT\n100.0\n")));
    QVERIFY(svc.importFolder(aFile, &err).isEmpty());
    QVERIFY(!err.isEmpty());

    const QString emptyDir = tmp.filePath(QStringLiteral("empty"));
    QVERIFY(QDir().mkpath(emptyDir));
    QVERIFY(svc.importFolder(emptyDir, &err).isEmpty());
    QVERIFY(!err.isEmpty());

    // 工程目录自身（或内部目录）不能当导入源。
    QVERIFY(svc.importFolder(projectDir, &err).isEmpty());
    QVERIFY(!err.isEmpty());

    // 包住工程目录的上级目录：工程产物子树不出行，只收外面的普通文件。
    const QVector<DataImportService::FolderRowResult> rows =
        svc.importFolder(tmp.path(), &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.front().path, aFile);
  }

  // 确认表后端：previewFolder 与 importFolder 同一枚举/行序（按行索引对齐），
  // typeOverrides 把行重定向到用户改过的类型（forceType 落进资产类型）。
  void folderPreviewMatchesImportOrderAndOverridesApply()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = DataImportService::FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井位"))));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井分层"))));
    QVERIFY(writeFile(QDir(root).filePath(QString::fromUtf8("井位/heads.dat")),
        QByteArrayLiteral("#WellHead File From SMI\n#Name X Y KB TD\n"
                          "A1  1.0  2.0  0.0  2000.0\n")));
    const QString topsPath =
        QDir(root).filePath(QString::fromUtf8("井分层/tops.dat"));
    QVERIFY(writeFile(topsPath,
        QByteArrayLiteral("#WellTops File From SMI\n#WellName  Name  MD\n"
                          "A1       D61   2148.0\n")));

    QString err;
    const auto preview = svc.previewFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(preview.size(), 2);
    // 与 importFolder 同一行序：well_head 先行。
    QVERIFY(preview.at(0).path.endsWith(QString::fromUtf8("井位/heads.dat")));
    QCOMPARE(preview.at(0).classifiedType, QStringLiteral("well_head"));
    QCOMPARE(preview.at(1).path, topsPath);
    QCOMPARE(preview.at(1).classifiedType,
             QStringLiteral("well_stratification"));
    QVERIFY(!preview.at(0).skipped && !preview.at(1).skipped);

    // 确认表「改类型」：tops.dat 改成 document → 不再走 tops 解析，
    // 落辅助实体 + reference 链接；行结果 classifiedType 反映新类型。
    QMap<QString, QString> overrides;
    overrides.insert(topsPath, QStringLiteral("document"));
    const auto rows = svc.importFolder(root, &err, overrides);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).path, preview.at(0).path); // 行序对齐预览
    QCOMPARE(rows.at(1).path, preview.at(1).path);
    QCOMPARE(rows.at(0).outcome, Outcome::Imported);
    QCOMPARE(rows.at(1).outcome, Outcome::Imported);
    QCOMPARE(rows.at(1).classifiedType, QStringLiteral("document"));

    bool sawDoc = false;
    for (const CatalogAsset &a : cat->assets())
      if (a.type == QLatin1String("document"))
        sawDoc = true;
    QVERIFY(sawDoc);
  }

  // T22/D5：改成 well_head 的行按生效类型回阶段 1——井建得够早，同一批里
  // 排在它后面的行照常挂到刚建的井。同时验证 importFolder 结果行序 = 生效
  // 类型序（override 后行序不再等于预览序，UI 按路径回行）。
  void folderImportRetypedWellHeadRejoinsPhaseOne()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = DataImportService::FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(root));
    // 路径段没有任何语义 → 分类器落 tabular；内容却是合法井口表。
    const QString headsPath = QDir(root).filePath(QStringLiteral("zheads.dat"));
    QVERIFY(writeFile(headsPath, QByteArrayLiteral(
        "#WellHead File From SMI\n#Name X Y KB TD\n"
        "B2  3.0  4.0  0.0  2100.0\n")));
    // LAS 按文件名排在 zheads.dat 前；不管顺序如何它是阶段 2，能不能挂上
    // 取决于 zheads.dat 是不是阶段 1。
    const QString lasPath = QDir(root).filePath(QStringLiteral("a.las"));
    QVERIFY(writeFile(lasPath, QByteArrayLiteral(
        "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. B2 : WELL\n"
        "~Curve\nDEPT.M :\n~A DEPT\n100.0\n")));

    QString err;
    const auto preview = svc.previewFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(preview.size(), 2);
    QCOMPARE(preview.at(0).path, lasPath); // 预览按分类器类型：两都行阶段 2，按路径排
    QCOMPARE(preview.at(1).classifiedType, QStringLiteral("tabular"));

    QMap<QString, QString> overrides;
    overrides.insert(headsPath, QStringLiteral("well_head"));
    const auto rows = svc.importFolder(root, &err, overrides);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 2);

    // 生效类型 = well_head → 回阶段 1：井口行在最前，LAS 随后并挂到 B2。
    QCOMPARE(rows.at(0).path, headsPath);
    QCOMPARE(rows.at(0).classifiedType, QStringLiteral("well_head"));
    QCOMPARE(rows.at(0).outcome, Outcome::Imported);
    QVERIFY(rows.at(0).entityName.contains(QStringLiteral("B2")));
    QCOMPARE(rows.at(1).path, lasPath);
    QCOMPARE(rows.at(1).classifiedType, QStringLiteral("well_log"));
    QCOMPARE(rows.at(1).outcome, Outcome::Imported);
    QCOMPARE(rows.at(1).entityName, QStringLiteral("B2"));

    QVERIFY(cat->hasEntity(QStringLiteral("well-B2")));
    int wellLogLinks = 0;
    for (const EntityAssetLink &l : cat->linksForEntity(QStringLiteral("well-B2")))
      if (l.role == QLatin1String("well_log") && l.isPrimary && !l.unresolved)
        ++wellLogLinks;
    QCOMPARE(wellLogLinks, 1);
  }

  // T22：override 只认分类器词表内的类型——非法值忽略，行按分类器原类型走；
  // 词表本身含 tabular/reference，不含 tops/auxiliary（角色名/兜底名不是类型）。
  void folderImportIgnoresInvalidOverrideType()
  {
    QVERIFY(!projectClassifierTypes().contains(QStringLiteral("tops")));
    QVERIFY(!projectClassifierTypes().contains(QStringLiteral("auxiliary")));
    QVERIFY(projectClassifierTypes().contains(QStringLiteral("well_stratification")));
    QVERIFY(projectClassifierTypes().contains(QStringLiteral("tabular")));
    QVERIFY(projectClassifierTypes().contains(QStringLiteral("reference")));
    QVERIFY(isClassifierType(QStringLiteral("well_log")));
    QVERIFY(!isClassifierType(QStringLiteral("not_a_type")));

    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = DataImportService::FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井分层"))));
    const QString topsPath = QDir(root).filePath(QString::fromUtf8("井分层/tops.dat"));
    QVERIFY(writeFile(topsPath, QByteArrayLiteral(
        "#WellTops File From SMI\n#WellName  Name  MD\n"
        "A1       D61   2148.0\n")));

    QString err;
    QMap<QString, QString> overrides;
    overrides.insert(topsPath, QStringLiteral("not_a_type")); // 非法 → 忽略
    const auto rows = svc.importFolder(root, &err, overrides);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.at(0).classifiedType, QStringLiteral("well_stratification"));
    QCOMPARE(rows.at(0).outcome, Outcome::Unresolved); // 无井 → 未决链接
    for (const CatalogAsset &a : cat->assets())
      QVERIFY(a.type != QLatin1String("not_a_type"));
  }

  // T22：固定辅助收窄到 HZ28-6-1 命名文件——「参考资料」目录内其他 XML 的
  // override 送达后端：改成 well_log → 照常按井解析（对不上 A1 保持未决），
  // 不再被整目录锁成参考；HZ28-6-1 自身给 override 也无效，仍落辅助参考。
  void folderImportReferenceDirXmlOverrideReachesBackend()
  {
    QVERIFY(!isFixedAuxiliaryPath(QString::fromUtf8("/a/参考资料/other.xml")));
    QVERIFY(isFixedAuxiliaryPath(QString::fromUtf8("/a/b/HZ28-6-1测井.xml")));
    QVERIFY(isDefaultReferencePath(QString::fromUtf8("/a/参考资料/doc.pdf")));
    QVERIFY(!isDefaultReferencePath(QString::fromUtf8("/a/普通/doc.pdf")));

    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    QVERIFY(seedCatalogWithSingleWell(projectDir)); // 预置 well-A1
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();
    using Outcome = DataImportService::FolderRowResult::Outcome;

    const QString root = tmp.filePath(QStringLiteral("area"));
    const QString refDir = QDir(root).filePath(QString::fromUtf8("参考资料"));
    QVERIFY(QDir().mkpath(refDir));
    const QByteArray logXml(
        "<logs><log><logcurveinfo/><logdata>1 2</logdata></log></logs>");
    const QByteArray logXml2(
        "<logs><log><logcurveinfo/><logdata>3 4</logdata></log></logs>");
    const QString otherPath = QDir(refDir).filePath(QStringLiteral("other.xml"));
    const QString hz28Path = QDir(refDir).filePath(QStringLiteral("HZ28-6-1综合.xml"));
    QVERIFY(writeFile(otherPath, logXml));
    QVERIFY(writeFile(hz28Path, logXml2)); // 不同字节——避免 dedup 互相吞掉

    QString err;
    QMap<QString, QString> overrides;
    overrides.insert(otherPath, QStringLiteral("well_log"));   // 参考→测井
    overrides.insert(hz28Path, QStringLiteral("well_head"));   // 锁住行：覆盖无效
    const auto rows = svc.importFolder(root, &err, overrides);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(rows.size(), 2);

    // other.xml：override 生效——没有辅助实体，按 well 侧留未决链接。
    QString otherAsset, hz28Asset;
    for (const CatalogAsset &a : cat->assets())
    {
      if (a.displayName == QStringLiteral("other.xml"))
        otherAsset = a.id;
      if (a.displayName == QStringLiteral("HZ28-6-1综合.xml"))
        hz28Asset = a.id;
    }
    QVERIFY(!otherAsset.isEmpty() && !hz28Asset.isEmpty());
    const auto otherLinks = cat->linksForAsset(otherAsset);
    QCOMPARE(otherLinks.size(), 1);
    QCOMPARE(otherLinks.front().entityType, QStringLiteral("well"));
    QVERIFY(otherLinks.front().unresolved);       // 「改成井之后如果对不上…保持未决」
    QVERIFY(otherLinks.front().entityId.isEmpty());
    const auto hz28Links = cat->linksForAsset(hz28Asset);
    QCOMPARE(hz28Links.size(), 1);
    QCOMPARE(hz28Links.front().entityType, QStringLiteral("auxiliary"));
    QCOMPARE(hz28Links.front().role, QStringLiteral("reference"));
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 1); // 只有预置 A1
    QCOMPARE(cat->entities(QStringLiteral("auxiliary")).size(), 1); // 仅 HZ28

    for (const auto &r : rows)
      QCOMPARE(r.outcome, r.path == otherPath ? Outcome::Unresolved : Outcome::Imported);
  }

  // T22：确认表「重试」的后端入口——单行按当前（覆盖）类型重导，行结果口径
  // 与 importFolder 相同；修文件后再导成功；非法 forceType 忽略。
  void importFolderRowReimportsSingleFile()
  {
    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    using Outcome = DataImportService::FolderRowResult::Outcome;

    const QString dir = tmp.filePath(QString::fromUtf8("井位"));
    QVERIFY(QDir().mkpath(dir));
    const QString headsPath = QDir(dir).filePath(QStringLiteral("heads.dat"));
    QVERIFY(writeFile(headsPath,
                      QByteArrayLiteral("#WellHead File From SMI\n# no rows\n")));

    QString err;
    DataImportService::FolderRowResult row =
        svc.importFolderRow(headsPath, QString(), &err);
    QCOMPARE(row.outcome, Outcome::Failed);
    QVERIFY(!err.isEmpty());
    QVERIFY(row.message.contains(QStringLiteral("no well head rows")));
    QCOMPARE(row.classifiedType, QStringLiteral("well_head"));

    // 修好文件再重导：入库 + 实体名，error 清空。
    QVERIFY(writeFile(headsPath, QByteArrayLiteral(
        "#WellHead File From SMI\n#Name X Y KB TD\n"
        "A1  1.0  2.0  0.0  2000.0\n")));
    row = svc.importFolderRow(headsPath, QString(), &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(row.outcome, Outcome::Imported);
    QVERIFY(row.entityName.contains(QStringLiteral("A1")));

    // forceType 口径同 importFolder 覆盖：合法值改类型，非法值忽略。
    const QString miscPath = tmp.filePath(QStringLiteral("misc.dat"));
    QVERIFY(writeFile(miscPath, QByteArrayLiteral("a,b\n1,2\n")));
    row = svc.importFolderRow(miscPath, QStringLiteral("document"), &err);
    QCOMPARE(row.classifiedType, QStringLiteral("document"));
    QCOMPARE(row.outcome, Outcome::Imported); // document → 辅助实体参考关联
    row = svc.importFolderRow(miscPath, QStringLiteral("bogus_type"), &err);
    QCOMPARE(row.classifiedType, QStringLiteral("tabular")); // 非法 → 分类器原类型
  }

  // 可选真数据：PALEO_REAL_PROJECT_AREA 跑一次 importFolder——井口先行后
  // A1 仍得四条主关联；无 Failed 行（与 tst_smoke_realdata 同一门禁变量）。
  void folderImportRealAreaSmoke()
  {
    const QString src = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
    if (src.isEmpty() || !QDir(src).exists())
      QSKIP("PALEO_REAL_PROJECT_AREA not set — folder real-data smoke skipped");

    QTemporaryDir tmp;
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    QString err;
    QElapsedTimer timer;
    timer.start();
    const QVector<DataImportService::FolderRowResult> rows = svc.importFolder(src, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QVERIFY(rows.size() >= 60); // 60 个数据文件 + 旧产物/工作区 json 参考行

    int nImported = 0, nUnresolved = 0, nFailed = 0, nSkipped = 0;
    bool sawWellHead = false, sawSeismic = false;
    for (const DataImportService::FolderRowResult &r : rows)
    {
      switch (r.outcome)
      {
        case DataImportService::FolderRowResult::Outcome::Imported: ++nImported; break;
        case DataImportService::FolderRowResult::Outcome::Unresolved: ++nUnresolved; break;
        case DataImportService::FolderRowResult::Outcome::Failed:
          ++nFailed;
          qWarning("FOLDER FAIL %s: %s", qPrintable(r.path), qPrintable(r.message));
          break;
        case DataImportService::FolderRowResult::Outcome::Skipped: ++nSkipped; break;
      }
      sawWellHead = sawWellHead || r.classifiedType == QLatin1String("well_head");
      sawSeismic = sawSeismic || r.classifiedType == QLatin1String("seismic");
    }
    qWarning("FOLDER SMOKE %lld ms: %d rows — 入库 %d, 未决 %d, 失败 %d, 跳过 %d",
             timer.elapsed(), rows.size(), nImported, nUnresolved, nFailed, nSkipped);
    QCOMPARE(nFailed, 0);
    QVERIFY(sawWellHead && sawSeismic);

    DataCatalog *cat = svc.catalog();
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), 20);
    QCOMPARE(cat->entities(QStringLiteral("sequence_boundary")).size(), 8);
    QCOMPARE(cat->entities(QStringLiteral("seismic_survey")).size(), 1);
    QStringList roles;
    for (const EntityAssetLink &l : cat->linksForEntity(QStringLiteral("well-A1")))
      if (l.isPrimary && !l.unresolved)
        roles.append(l.role);
    std::sort(roles.begin(), roles.end());
    QCOMPARE(roles, QStringList({QStringLiteral("time_depth"), QStringLiteral("tops"),
                                 QStringLiteral("well_head"), QStringLiteral("well_log")}));
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
