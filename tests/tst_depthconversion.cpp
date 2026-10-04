// 层：测试壳
#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <memory>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/wellfileparsers.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/workflow/depthconversionworkflow.h"

#include <gdal.h>

#include <qgsproject.h>

// goal/time-depth-velocity 轮3：时深转换全链验收（Oracle 第 1/2/3 条）——
// 导入（井位/分层/校验炮/层位）→ catalog 关联收集 → 建模存档 → 层位时间栅格
// → 深度栅格 DERIVED 登记 + 层声明，全程 offscreen 真服务栈驱动。
// 井锚点零误差在链上钉死：A1 井位查校验炮行 TWT 必须位级命中 TVD。
class TestDepthConversion : public QObject
{
  Q_OBJECT

  struct Stack
  {
    QgisProjectService projectSvc;
    std::unique_ptr<LayerManifest> manifest;
    std::unique_ptr<QgisLayerService> layerSvc;
    std::unique_ptr<PaleoProjectStore> store;
    std::unique_ptr<DataImportService> importSvc;
  };

  static std::unique_ptr<Stack> makeStack(const QString &projectDir)
  {
    auto s = std::make_unique<Stack>();
    const QString metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
    if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
      return nullptr;
    s->manifest = std::make_unique<LayerManifest>(metaPath);
    if (!s->manifest->open())
      return nullptr;
    s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
    s->store = std::make_unique<PaleoProjectStore>();
    s->store->setProjectPaths(QDir(projectDir).filePath(QStringLiteral("proj.qgz")),
                              QDir(projectDir).filePath(QStringLiteral("project.gpkg")),
                              metaPath);
    s->importSvc = std::make_unique<DataImportService>(s->store.get());
    QObject::connect(s->importSvc.get(), &DataImportService::layerDeclared,
                     s->layerSvc.get(), [layerSvc = s->layerSvc.get()](const LayerDeclaration &decl) {
                       QString err;
                       layerSvc->declare(decl, &err);
                     });
    s->importSvc->setProjectDir(projectDir);
    return s;
  }

  static QString fixture(const QString &name)
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
  }

  // 摆进带语义的目录再导入（分类按路径段：井位/井分层/时深/层位）。
  static QString stageFixture(const QTemporaryDir &tmp, const QString &dir, const QString &name,
                              const QString &asName = QString())
  {
    const QString d = tmp.filePath(dir);
    if (!QDir().mkpath(d))
      return QString();
    const QString dst = QDir(d).filePath(asName.isEmpty() ? name : asName);
    if (!QFile::copy(fixture(name), dst))
      return QString();
    return dst;
  }

  static QByteArray readFile(const QString &path)
  {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
  }

  void init()
  {
    QgsProject::instance()->clear();
  }

private slots:
  void fullChainBuildsModelAndConvertsHorizon();
  void repeatedBuildAndConversionAreIdempotent();
  void latestModelPathUsesCommitOrderAcrossAssets();
};

void TestDepthConversion::fullChainBuildsModelAndConvertsHorizon()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack != nullptr);
  DataCatalog *catalog = stack->importSvc->catalog();

  // ---- 导入四类源数据（井位先行：井实体建齐后其余按名解析） ----
  QString err;
  QStringList staged;
  staged << stageFixture(tmp, QStringLiteral("井位"), QStringLiteral("ExportWellHead.dat"));
  staged << stageFixture(tmp, QStringLiteral("井分层"), QStringLiteral("DC.dat"));
  staged << stageFixture(tmp, QStringLiteral("时深"), QStringLiteral("A1_TD.dat"));
  // D61_sample.dat 摆成 D61.dat：stem 须在 AreaRules.sequenceBoundaries 名单内
  // （未决层位不派生时间栅格/不声明图层——导入侧既有语义）。
  staged << stageFixture(tmp, QStringLiteral("层位"), QStringLiteral("D61_sample.dat"),
                        QStringLiteral("D61.dat"));
  for (const QString &p : staged)
  {
    err.clear();
    QVERIFY2(!stack->importSvc->importProjectFile(p, &err).isEmpty(),
             qPrintable(QStringLiteral("%1: %2").arg(p, err)));
  }

  // ---- catalog 关联收集：tops/time_depth/well_head 各就位 ----
  const VelocityModelBuildRequest req =
      DepthConversionWorkflow::requestFromCatalog(catalog, projectDir);
  QCOMPARE(req.topsFilePaths.size(), 1);
  QCOMPARE(req.tdFilePaths.size(), 1);
  QCOMPARE(req.wellHeadFilePaths.size(), 1);
  QVERIFY(QFile::exists(req.tdFilePaths.first()));

  // ---- 建模 + 存档 ----
  DepthConversionWorkflow workflow(catalog, projectDir);
  workflow.setLayerService(stack->layerSvc.get());
  QSignalSpy modelSpy(&workflow, &DepthConversionWorkflow::modelStored);
  const QString modelPath = workflow.buildAndStoreModel(req, &err);
  QVERIFY2(!modelPath.isEmpty(), qPrintable(err));
  QCOMPARE(modelSpy.count(), 1);
  QVERIFY(QFile::exists(modelPath));
  QCOMPARE(DepthConversionWorkflow::latestModelPath(catalog, projectDir), modelPath);

  QString loadErr;
  const paleo::velmodel::VelocityModel model =
      DepthConversionWorkflow::loadModel(modelPath, &loadErr);
  QVERIFY2(model.isValid(), qPrintable(loadErr));
  QCOMPARE(model.wells().size(), 1);
  QCOMPARE(model.wells().first().id, QStringLiteral("A1"));
  QVERIFY(model.wells().first().knots.size() >= 400); // A1_TD.dat 全表行数级别

  // ---- 链上锚点零误差（Oracle 第 2 条）：A1 井位查校验炮 TWT 位级命中 TVD ----
  const TimeDepthTable td = parseTimeDepthText(readFile(fixture(QStringLiteral("A1_TD.dat"))));
  QVERIFY(td.rows.size() > 400);
  const double a1x = 5288.670, a1y = 8219.940; // ExportWellHead.dat A1 行
  const qsizetype nRows = td.rows.size();
  for (int i : {0, 1, int(nRows / 2), int(nRows - 2), int(nRows - 1)})
  {
    const TdRow &row = td.rows[i];
    if (!(row.timeMs > 0.0) || !(row.tvd > 0.0))
      continue;
    QCOMPARE(model.depthForTwt(a1x, a1y, row.timeMs), row.tvd); // 位级相等
  }

  // ---- 层位时间栅格 → 深度栅格 + 层声明 ----
  LayerDeclaration timeDecl;
  bool foundTime = false;
  for (const LayerDeclaration &d : stack->manifest->all())
    if (d.layerId == QLatin1String("horizon.D61"))
    {
      timeDecl = d;
      foundTime = true;
    }
  QVERIFY(foundTime);
  QVERIFY(QFile::exists(timeDecl.source));

  QSignalSpy doneSpy(&workflow, &DepthConversionWorkflow::conversionDone);
  QString layerId;
  QVERIFY2(workflow.convertRasterToDepth(QStringLiteral("D61"), timeDecl.source,
                                          modelPath, &layerId, &err),
           qPrintable(err));
  QCOMPARE(layerId, QStringLiteral("depth.D61"));
  QCOMPARE(doneSpy.count(), 1);

  LayerDeclaration depthDecl;
  bool foundDepth = false;
  for (const LayerDeclaration &d : stack->manifest->all())
    if (d.layerId == QLatin1String("depth.D61"))
    {
      depthDecl = d;
      foundDepth = true;
    }
  QVERIFY(foundDepth);
  QCOMPARE(depthDecl.type, QStringLiteral("raster"));
  QCOMPARE(depthDecl.group, QStringLiteral("00_Data"));
  QVERIFY(QFile::exists(depthDecl.source));

  // ---- catalog 侧：velocity_model 与 depth_raster 资产都在，转换计数入 extra ----
  int velocityAssets = 0, depthAssets = 0;
  QVariantMap depthExtra;
  for (const CatalogAsset &asset : catalog->assets())
  {
    if (asset.type == QLatin1String("velocity_model"))
      ++velocityAssets;
    if (asset.type == QLatin1String("depth_raster"))
    {
      ++depthAssets;
      const QVector<CatalogVersion> versions = catalog->versionsForAsset(asset.id);
      QVERIFY(!versions.isEmpty());
      depthExtra = versions.constLast().extra;
    }
  }
  QCOMPARE(velocityAssets, 1);
  QCOMPARE(depthAssets, 1);
  QVERIFY(depthExtra.value(QStringLiteral("converted_cells")).toInt() > 0);

  // ---- 栅格回读：深度像元与模型逐点一致（执行器没有走样） ----
  GDALAllRegister();
  GDALDatasetH ds = GDALOpen(depthDecl.source.toUtf8().constData(), GA_ReadOnly);
  QVERIFY2(ds != nullptr, "depth GeoTIFF 可开");
  const int cols = GDALGetRasterXSize(ds), rows = GDALGetRasterYSize(ds);
  double gt[6] = {0, 0, 0, 0, 0, 0};
  GDALGetGeoTransform(ds, gt);
  QVector<float> depth(rows * cols);
  QCOMPARE(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, 0, 0, cols, rows,
                        depth.data(), cols, rows, GDT_Float32, 0, 0),
           CE_None);
  int nodataFlag = 0;
  const double nodata = GDALGetRasterNoDataValue(GDALGetRasterBand(ds, 1), &nodataFlag);
  GDALClose(ds);

  // 时间栅格同格回读，抽 3 个有值像元验证 z == model.depthForTwt(格心, t)。
  GDALDatasetH tds = GDALOpen(timeDecl.source.toUtf8().constData(), GA_ReadOnly);
  QVERIFY(tds != nullptr);
  QVector<float> time(rows * cols);
  QCOMPARE(GDALRasterIO(GDALGetRasterBand(tds, 1), GF_Read, 0, 0, cols, rows,
                        time.data(), cols, rows, GDT_Float32, 0, 0),
           CE_None);
  GDALClose(tds);

  int checked = 0;
  for (int row = 0; row < rows && checked < 3; ++row)
    for (int col = 0; col < cols && checked < 3; ++col)
    {
      const int idx = row * cols + col;
      if (time[idx] == -9999.0f || !std::isfinite(time[idx]))
        continue;
      const double cx = gt[0] + (col + 0.5) * gt[1];
      const double cy = gt[3] + (row + 0.5) * gt[5];
      const double expected = model.depthForTwt(cx, cy, static_cast<double>(time[idx]));
      if (std::isnan(expected))
      {
        QCOMPARE(static_cast<double>(depth[idx]), nodata);
        continue;
      }
      QCOMPARE(depth[idx], static_cast<float>(expected)); // 位级一致（float 域）
      ++checked;
    }
  QVERIFY(checked == 3);
}

// Oracle 第 3 条：重复建模/重复转换幂等——同资产递增版本，文件字节一致。
void TestDepthConversion::repeatedBuildAndConversionAreIdempotent()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack != nullptr);
  DataCatalog *catalog = stack->importSvc->catalog();

  QString err;
  const QString wellHead = stageFixture(tmp, QStringLiteral("井位"), QStringLiteral("ExportWellHead.dat"));
  const QString td = stageFixture(tmp, QStringLiteral("时深"), QStringLiteral("A1_TD.dat"));
  const QString horizon = stageFixture(tmp, QStringLiteral("层位"),
                                        QStringLiteral("D61_sample.dat"),
                                        QStringLiteral("D61.dat"));
  for (const QString &p : {wellHead, td, horizon})
    QVERIFY(!stack->importSvc->importProjectFile(p, &err).isEmpty());

  VelocityModelBuildRequest req;
  req.tdFilePaths << td;
  req.wellHeadFilePaths << wellHead;
  DepthConversionWorkflow workflow(catalog, projectDir);
  workflow.setLayerService(stack->layerSvc.get());
  const QString model1 = workflow.buildAndStoreModel(req, &err);
  const QString model2 = workflow.buildAndStoreModel(req, &err);
  QVERIFY(!model1.isEmpty());
  QVERIFY(!model2.isEmpty());
  QVERIFY(model1 != model2); // 同资产两个递增版本目录
  QCOMPARE(readFile(model1), readFile(model2)); // 模型字节幂等（无时间戳）

  LayerDeclaration timeDecl;
  for (const LayerDeclaration &d : stack->manifest->all())
    if (d.layerId == QLatin1String("horizon.D61"))
      timeDecl = d;
  QVERIFY(!timeDecl.source.isEmpty());

  QString layerId;
  QVERIFY(workflow.convertRasterToDepth(QStringLiteral("D61"), timeDecl.source,
                                        model1, &layerId, &err));
  const QString depth1 = [&stack]() {
    for (const LayerDeclaration &d : stack->manifest->all())
      if (d.layerId == QLatin1String("depth.D61"))
        return d.source;
    return QString();
  }();
  // 第二次转换：同一 depth_raster 资产新增版本，层声明指向新版；两版字节一致。
  QVERIFY(workflow.convertRasterToDepth(QStringLiteral("D61"), timeDecl.source,
                                        model1, &layerId, &err));
  QString depth2;
  int depthVersions = 0;
  for (const CatalogAsset &asset : catalog->assets())
    if (asset.type == QLatin1String("depth_raster"))
      depthVersions = catalog->versionsForAsset(asset.id).size();
  QCOMPARE(depthVersions, 2);
  for (const LayerDeclaration &d : stack->manifest->all())
    if (d.layerId == QLatin1String("depth.D61"))
      depth2 = d.source;
  QVERIFY(!depth1.isEmpty() && !depth2.isEmpty());
  QCOMPARE(readFile(depth1), readFile(depth2)); // 像元位级幂等 → 文件字节一致
}

// #127：层间平均 v1、v2 后再建 V0-k v1——最新应是后建的 V0-k（旧代码跨资产比
// versionNumber，永远选层间平均 v2）。重开 catalog 后结论不变（行序稳定）。
void TestDepthConversion::latestModelPathUsesCommitOrderAcrossAssets()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  QString err;
  QString newest;
  {
    DataCatalog cat;
    QVERIFY2(cat.open(tmp.path(), &err), qPrintable(err));
    DerivedAssetRegistrar registrar(&cat, tmp.path());
    const auto model = [&](const QString &name) {
      const DerivedStaging st =
          registrar.stage(QStringLiteral("velocity_model"), name, QStringLiteral("model.json"), &err);
      if (!st.isValid())
        return QString();
      QFile f(st.absolutePath);
      if (!f.open(QIODevice::WriteOnly))
        return QString();
      f.write("{}");
      f.close();
      if (!registrar.commit(st, {}, QStringLiteral("probe"), {}, &err))
        return QString();
      return st.absolutePath;
    };
    QVERIFY2(!model(QStringLiteral("layer-average")).isEmpty(), qPrintable(err));
    QVERIFY2(!model(QStringLiteral("layer-average")).isEmpty(), qPrintable(err));
    newest = model(QStringLiteral("v0k"));
    QVERIFY2(!newest.isEmpty(), qPrintable(err));
    QCOMPARE(QFileInfo(DepthConversionWorkflow::latestModelPath(&cat, tmp.path())).canonicalFilePath(),
             QFileInfo(newest).canonicalFilePath());
  }
  DataCatalog reopened;
  QVERIFY2(reopened.open(tmp.path(), &err), qPrintable(err));
  QCOMPARE(QFileInfo(DepthConversionWorkflow::latestModelPath(&reopened, tmp.path())).canonicalFilePath(),
           QFileInfo(newest).canonicalFilePath());
}


int main(int argc, char *argv[])
{
  // Offscreen QGIS bootstrap through the runtime that owns init order.
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestDepthConversion tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_depthconversion.moc"
