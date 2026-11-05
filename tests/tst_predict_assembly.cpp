// 层：数据（测试壳位于 tests/，被测对象为 ui 装配缝 + workflow 任务面）
// #277 装配缝回归：预测页 runRequested 三段式——prepare（GUI 线程，catalog
// stage）→ compute（worker，只跑 Processing / ONNX 推理 + 栅格落盘，不碰
// catalog）→ publish（GUI，commitExternal + 图层声明 + predictionDone）。
// 修前 runPrediction 整体进 worker，派生登记被 catalog 线程闸拒绝（任务恒
// 失败）、outErr 从不写入（失败原因恒空）。本套件经真实 PaleoMainWindow +
// attachWorkflows + PaleoTaskService 驱动端到端形状。
#include "helpers/workflowfixture.h"
#include <QtTest>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLabel>
#include <QVariantMap>

#include <functional>

#include <gdal.h>
#include <cpl_conv.h>
#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <qgsmaplayer.h>
#include <qgsrasterlayer.h>

#include "../src/catalog/datacatalog.h"
#include "../src/io/areamodel.h"
#include "../src/linkage/selectioncontext.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/stratigraphicsubintervals.h"
#include "../src/ui/pages/pagepanels.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/workflow/workflows.h"
#include <paleo_data/service.h>

class TestPredictAssembly : public QObject
{
  Q_OBJECT

private:
  using Fixture = paleo::tests::WorkflowFixture;

  // Write a w x h Float32 GTiff; returns "" on failure (tst_workflows 同法).
  static QString makeRaster(const QString &path, int w, int h, const QVector<float> &px)
  {
    GDALDriverH drv = GDALGetDriverByName("GTiff");
    GDALDatasetH ds = GDALCreate(drv, path.toUtf8().constData(), w, h, 1, GDT_Float32, nullptr);
    if (!ds)
      return QString();
    const double gt[6] = {0.0, 1.0, 0.0, static_cast<double>(h), 0.0, -1.0};
    GDALSetGeoTransform(ds, const_cast<double *>(gt));
    const auto crs=QgsCoordinateReferenceSystem::fromEpsgId(3857).toWkt().toUtf8();
    GDALSetProjection(ds,crs.constData());
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    GDALSetRasterNoDataValue(band, -9999.0);
    const CPLErr err = GDALRasterIO(band, GF_Write, 0, 0, w, h, const_cast<float *>(px.constData()),
                                    w, h, GDT_Float32, 0, 0);
    GDALClose(ds);
    return err == CE_None ? path : QString();
  }

  // 层声明小 helper（tst_workflows 同法：00_Test 组）。
  static LayerDeclaration decl(const QString &layerId, const QString &horizon,
                               const QString &type, const QString &source)
  {
    LayerDeclaration d;
    d.layerId = layerId;
    d.horizon = horizon;
    d.type = type;
    d.source = source;
    d.group = QStringLiteral("00_Test");
    return d;
  }

  // The shell requires a persisted analysis unit. The legacy horizon argument
  // neither selects the unit nor supplies the input raster's analysis identity.
  static QgsMapLayer *makeScopedInput(Fixture &f, SelectionContext &selection, QString *error)
  {
    paleo::data::DataService data;std::string detail;
    // initFixture's project service already owns and creates this area model.
    if (!data.openProject(paleo::area_model::directory(f.dir.path()).toStdString(),detail))
    {
      if (error)*error=QString::fromStdString(detail);return nullptr;
    }
    const auto draft=paleo::strat::draftScheme("prediction-assembly","assembly",{"T1","T2"},{});
    paleo::data::StratigraphicScheme scheme;std::vector<paleo::data::FrameworkIssue> issues;
    if (!data.commitStratigraphicScheme(draft,{},scheme,issues,detail))
    {
      if (error)*error=QString::fromStdString(detail);return nullptr;
    }
    const auto options=paleo::strat::selectorOptions(scheme);
    if (options.isEmpty())return nullptr;
    selection.setActiveUnit(paleo::strat::makeSelection(scheme,options.front().intervalId,1));
    data.closeProject();
    if (!selection.activeUnit().isValid() || selection.activeUnit().revision.isEmpty())return nullptr;
    const auto key=paleo::strat::analysisLayerKey(selection.activeUnit());
    const auto path=makeRaster(f.dir.filePath("coded_in.tif"),3,3,{1,1,1,1,2,1,1,1,1});
    if (path.isEmpty())return nullptr;
    CatalogAsset asset;asset.id="ast-input";asset.type="single_factor_raster";
    if (!f.catalog.addAsset(asset,error))return nullptr;
    CatalogVersion version;version.id="ver-input";version.assetId=asset.id;version.stage="DERIVED";
    version.managed=false;version.path=path;version.sourceUri=path;version.fileName="coded_in.tif";
    QFile file(path);if (!file.open(QIODevice::ReadOnly))return nullptr;
    version.sha256=QString::fromLatin1(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256).toHex());
    version.extra.insert("unitSelection",paleo::strat::toMap(selection.activeUnit()));
    if (!f.catalog.addVersion(version,error) ||
        !f.layers.declare(decl("input."+key,key,"raster",path),error))return nullptr;
    return f.layers.instantiate("input."+key,error);
  }

  // 轮询任务注册表：标题含 needle 的任务到达终态即返回。
  static PaleoTask *waitForTask(PaleoTaskService &svc, const QString &needle,
                                int timeoutMs = 60000)
  {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() <= timeoutMs)
    {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      for (PaleoTask *t : svc.tasks())
        if (t->title().contains(needle) && t->isFinished())
          return t;
      QTest::qWait(10);
    }
    return nullptr;
  }

  static QString statusTextOf(QWidget *page)
  {
    auto *status = page->findChild<QLabel *>(QStringLiteral("statusLabel"));
    return status ? status->text() : QString();
  }

private slots:

  void initTestCase()
  {
    QVERIFY(QgsApplication::instance() != nullptr);
    // Fixture::proc 注册 PaleoProvider（真算法）。
    Fixture f;
    QVERIFY(paleo::tests::initFixture(f));
    QVERIFY(f.proc.paleoAlgorithmIds().contains(QStringLiteral("paleo:paleo_geological_smoothing")));
  }

  // #277 成功路径：emit runRequested → 任务 Succeeded、catalog 出现 DERIVED
  // 版本、图层声明落地、线程闸零新增违规。
  void predictRunLandsDerivedVersionWithoutThreadViolations()
  {
    Fixture f;
    QVERIFY(paleo::tests::initFixture(f));

    SelectionContext selection;QString err;
    QgsMapLayer *input=makeScopedInput(f,selection,&err);
    QVERIFY2(input != nullptr, qPrintable(err));
    const auto analysisKey=paleo::strat::analysisLayerKey(selection.activeUnit());

    PredictionWorkflow wf(&f.proc, &f.layers);
    wf.setCatalog(&f.catalog, f.dir.path());

    const int violationsBefore = DataCatalog::threadViolationCount();

    PaleoTaskService taskSvc(&f.store);
    PaleoMainWindow win(nullptr, nullptr, nullptr, nullptr, &selection);
    win.attachWorkflows(&wf, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                        nullptr, nullptr, &taskSvc);
    auto *predictPage = win.findChild<PredictPage *>();
    QVERIFY(predictPage);

    QVariantMap params;
    params.insert(QStringLiteral("INPUT"), QVariant::fromValue(input));
    params.insert(QStringLiteral("PASSES"), 1);
    emit predictPage->runRequested(QStringLiteral("T1"),
                                   QStringLiteral("paleo:paleo_geological_smoothing"), params);
    QVERIFY2(!taskSvc.tasks().isEmpty(),qPrintable(statusTextOf(predictPage)));

    PaleoTask *task = waitForTask(taskSvc, QStringLiteral("paleo_geological_smoothing"));
    QVERIFY2(task != nullptr, "预测任务未在超时内结束");
    QCOMPARE(task->state(), PaleoTask::State::Succeeded);

    // publish 段（GUI 线程）：DERIVED 版本 + 图层声明 + predictionDone。
    bool declared = false;
    for (const LayerDeclaration &d : f.layers.declared())
      if (d.layerId == QStringLiteral("predict.%1.paleo.paleo_geological_smoothing").arg(analysisKey))
      {
        QCOMPARE(d.horizon,analysisKey);
        declared = true;
      }
    QVERIFY2(declared, "预测结果层未声明");
    bool derived = false;
    for (const CatalogAsset &asset : f.catalog.assets())
    {
      if (asset.type != QStringLiteral("prediction_raster"))
        continue;
      for (const CatalogVersion &dv : f.catalog.versionsForAsset(asset.id))
        if (dv.stage == QStringLiteral("DERIVED") && !dv.sha256.isEmpty())
        {
          QCOMPARE(paleo::strat::fromMap(dv.extra.value("unitSelection").toMap()),selection.activeUnit());
          QCOMPARE(dv.extra.value("source_horizon").toString(),analysisKey);
          QVERIFY(dv.parentVersionIds.contains("ver-input"));
          derived = true;
        }
    }
    QVERIFY2(derived, "预测产物未登记 DERIVED 版本");
    QVERIFY2(statusTextOf(predictPage).contains(QStringLiteral("预测完成")),
             qPrintable(statusTextOf(predictPage)));
    QCOMPARE(DataCatalog::threadViolationCount(), violationsBefore);
  }

  // #277 失败路径：compute 失败时任务 errorText 须带具体原因（修前 outErr
  // 从不写入，状态栏/日志恒为空的「预测失败：」）。
  void predictFailureSurfacesConcreteReason()
  {
    Fixture f;
    QVERIFY(paleo::tests::initFixture(f));
    SelectionContext selection;QString err;
    QgsMapLayer *input=makeScopedInput(f,selection,&err);
    QVERIFY2(input != nullptr,qPrintable(err));

    PredictionWorkflow wf(&f.proc, &f.layers);
    wf.setCatalog(&f.catalog, f.dir.path());
    PaleoTaskService taskSvc(&f.store);
    PaleoMainWindow win(nullptr, nullptr, nullptr, nullptr, &selection);
    win.attachWorkflows(&wf, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                        nullptr, nullptr, &taskSvc);
    auto *predictPage = win.findChild<PredictPage *>();
    QVERIFY(predictPage);

    // A scoped input lets preparation finish; the missing algorithm still fails
    // in compute, so this continues to exercise task error propagation.
    emit predictPage->runRequested(QStringLiteral("T1"), QStringLiteral("paleo:no_such_algorithm"),
                                   {{QStringLiteral("INPUT"),QVariant::fromValue(input)}});
    QVERIFY2(!taskSvc.tasks().isEmpty(),qPrintable(statusTextOf(predictPage)));

    PaleoTask *task = waitForTask(taskSvc, QStringLiteral("no_such_algorithm"));
    QVERIFY2(task != nullptr, "预测任务未在超时内结束");
    QCOMPARE(task->state(), PaleoTask::State::Failed);
    QVERIFY2(!task->errorText().isEmpty(), "任务 errorText 必须带具体原因");

    const QString status = statusTextOf(predictPage);
    QVERIFY2(status.contains(QStringLiteral("预测失败")), qPrintable(status));
    QVERIFY2(status.contains(QStringLiteral("no processing algorithm")), qPrintable(status));
  }
};

int main(int argc, char *argv[])
{
  if (qgetenv("QT_QPA_PLATFORM").isEmpty())
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QgsApplication app(argc, argv, false);
  app.setPrefixPath(qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr")), true);
  app.initQgis();
  QgsApplication::processingRegistry(); // ensure registry alive
  GDALAllRegister();
  TestPredictAssembly tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgsApplication::exitQgis();
  return rc;
}

#include "tst_predict_assembly.moc"
