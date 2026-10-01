// W7 收口测试：workflow/folderimport 编排契约（docs/UI_LAYER_PLAN.md W7.4）。
// 覆盖：previewFolder 只列不导、importFolder 同步路径 + importActiveChanged
// 双发、importFolderRow 重试、importFile 同步路径、importedWellHeadAsset
// 文件名回查、projectopen 的 stampSourceArea 目录门控。
#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectfile.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/workflow/folderimport.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/dialogs/folderconfirm.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include "../src/workflow/projectopen.h"

class TestFolderImport : public QObject
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

  static QString fixture(const QString &name)
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
  }

  static QString stageFixture(const QTemporaryDir &tmp, const QString &dir,
                              const QString &name, const QString &asName = QString())
  {
    const QString d = tmp.filePath(dir);
    if (!QDir().mkpath(d))
      return QString();
    const QString dst = QDir(d).filePath(asName.isEmpty() ? name : asName);
    return QFile::copy(fixture(name), dst) ? dst : QString();
  }

  static std::unique_ptr<Stack> makeStack(const QString &projectDir)
  {
    auto s = std::make_unique<Stack>();
    s->metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
    if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
      return nullptr;
    s->manifest = std::make_unique<LayerManifest>(s->metaPath);
    QString err;
    if (!s->manifest->open(&err))
      return nullptr;
    s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
    s->store = std::make_unique<PaleoProjectStore>();
    s->store->setProjectPaths(QDir(projectDir).filePath(QStringLiteral("proj.qgz")),
                              QDir(projectDir).filePath(QStringLiteral("project.gpkg")),
                              s->metaPath);
    s->importSvc = std::make_unique<DataImportService>(s->store.get());
    QObject::connect(s->importSvc.get(), &DataImportService::layerDeclared,
                     s->layerSvc.get(), [layerSvc = s->layerSvc.get()](const LayerDeclaration &decl) {
                       QString err;
                       layerSvc->declare(decl, &err);
                     });
    s->importSvc->setProjectDir(projectDir);
    return s;
  }

private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  // previewFolder 只列行不入库（确认表数据源的纯查询契约）。
  void previewListsWithoutImport()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString srcDir = tmp.filePath(QStringLiteral("src_area"));
    QVERIFY(!stageFixture(tmp, QStringLiteral("src_area"),
                          QStringLiteral("ExportWellHead.dat")).isEmpty());
    QVERIFY(!stageFixture(tmp, QStringLiteral("src_area"),
                          QStringLiteral("A1.Las")).isEmpty());

    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    FolderImportWorkflow wf(stack->importSvc.get(), nullptr);
    QString err;
    const QVector<FolderPreviewRow> rows = wf.previewFolder(srcDir, &err);
    QVERIFY2(!rows.isEmpty(), qPrintable(err));
    // 行已带分类类型与显示文案（displayType 由 preview 阶段回填）。
    bool sawTyped = false;
    for (const FolderPreviewRow &r : rows)
      if (!r.displayType.isEmpty())
        sawTyped = true;
    QVERIFY(sawTyped);
    // 纯查询：catalog 没有新资产。
    QVERIFY(stack->importSvc->catalog()->assets().isEmpty());
  }

  // 空目录 → 空行集（错误可为空——空表契约由确认表呈现）。
  void previewEmptyDir()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    FolderImportWorkflow wf(stack->importSvc.get(), nullptr);
    const QString emptyDir = tmp.filePath(QStringLiteral("empty"));
    QVERIFY(QDir().mkpath(emptyDir));
    QString err;
    const QVector<FolderPreviewRow> rows = wf.previewFolder(emptyDir, &err);
    QVERIFY(rows.isEmpty());
  }

  // 无任务服务 → 同步路径：done 在调用返回前已触发；importActiveChanged
  // 双发 true→false。
  void importFolderSyncPathTogglesActive()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(!stageFixture(tmp, QStringLiteral("src_area"),
                          QStringLiteral("ExportWellHead.dat")).isEmpty());
    QVERIFY(!stageFixture(tmp, QStringLiteral("src_area"),
                          QStringLiteral("A1.Las")).isEmpty());

    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    FolderImportWorkflow wf(stack->importSvc.get(), nullptr);
    QSignalSpy activeSpy(&wf, &FolderImportWorkflow::importActiveChanged);

    bool doneCalled = false;
    QVector<FolderRowResult> rows;
    QString importErr;
    wf.importFolder(tmp.filePath(QStringLiteral("src_area")), {},
                    [&doneCalled, &rows, &importErr](
                        const QVector<FolderRowResult> &r, const QString &e) {
                      doneCalled = true;
                      rows = r;
                      importErr = e;
                    });
    QVERIFY(doneCalled); // 同步路径：返回前已回调
    QVERIFY2(importErr.isEmpty(), qPrintable(importErr));
    QVERIFY(!rows.isEmpty());
    QCOMPARE(activeSpy.count(), 2);
    QCOMPARE(activeSpy.at(0).at(0).toBool(), true);
    QCOMPARE(activeSpy.at(1).at(0).toBool(), false);
    // 入库生效：catalog 有资产（井口 + LAS）。
    QVERIFY(!stack->importSvc->catalog()->assets().isEmpty());
  }

  // 文件名回查：导入后的井口资产按行源文件名定位。
  void importedWellHeadAssetResolvesByName()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString staged =
        stageFixture(tmp, QStringLiteral("src_area"),
                     QStringLiteral("ExportWellHead.dat"));
    QVERIFY(!staged.isEmpty());

    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    FolderImportWorkflow wf(stack->importSvc.get(), nullptr);
    QCOMPARE(wf.importedWellHeadAsset(staged), QString()); // 导入前查无
    QString err;
    QVERIFY(!stack->importSvc->importProjectFile(staged, &err).isEmpty());
    const QString assetId = wf.importedWellHeadAsset(staged);
    QVERIFY(!assetId.isEmpty());
    // 回查的是 well_head 资产
    bool found = false;
    for (const auto &a : stack->importSvc->catalog()->assets())
      if (a.id == assetId)
      {
        found = true;
        QCOMPARE(a.type, QStringLiteral("well_head"));
      }
    QVERIFY(found);
  }

  // importFolderRow：forceType 覆盖分类（视图按当前下拉值算好后传入）。
  void importFolderRowForceType()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString staged =
        stageFixture(tmp, QStringLiteral("src_area"),
                     QStringLiteral("A1_TD.dat"));
    QVERIFY(!staged.isEmpty());

    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    // 井先建齐（TD 的身份匹配需要井实体在库——单 TD 无井是 Unresolved，
    // 不是 Imported）。该用例在补上 private slots 前从未真正执行，恢复
    // 执行后按两阶段语义修正断言前提。
    QString herr;
    QVERIFY(!stack->importSvc
                  ->importProjectFile(
                      fixture(QStringLiteral("ExportWellHead.dat")), &herr)
                  .isEmpty());

    FolderImportWorkflow wf(stack->importSvc.get(), nullptr);
    QSignalSpy activeSpy(&wf, &FolderImportWorkflow::importActiveChanged);
    QString err;
    const FolderRowResult res =
        wf.importFolderRow(staged, QStringLiteral("time_depth"), &err);
    QVERIFY2(res.outcome == FolderRowResult::Outcome::Imported,
             qPrintable(err + QLatin1Char(' ') + res.message));
    // 路径键对账：结果行携源路径，确认表按它回写。
    QCOMPARE(res.path, staged);
    Q_UNUSED(activeSpy);
  }

  // importFile 同步路径（无任务服务）：返回 assetId 非空。
  void importFileSyncPath()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    FolderImportWorkflow wf(stack->importSvc.get(), nullptr);
    bool doneCalled = false;
    QString assetId, err;
    wf.importFile(QStringLiteral("well_log"), fixture(QStringLiteral("A1.Las")),
                  [&doneCalled, &assetId, &err](const QString &id, const QString &e) {
                    doneCalled = true;
                    assetId = id;
                    err = e;
                  });
    QVERIFY(doneCalled);
    QVERIFY2(!assetId.isEmpty(), qPrintable(err));
  }

  static QVector<FolderPreviewRow> wfPreview(Stack &stack, const QString &dir)
  {
    FolderImportWorkflow wf(stack.importSvc.get(), nullptr);
    QString err;
    return wf.previewFolder(dir, &err);
  }

  // stampSourceArea 门控：工程目录 != 导入目录时静默拒写；
  // 相等才回填 project.paleo 的 sourceAreaRoot。
  void stampSourceAreaDirGate()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));

    QgisProjectService projSvc;
    QVERIFY(projSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))));
    QVERIFY(QFile::exists(paleoProjectFilePath(projectDir)));

    ProjectOpenWorkflow wf(&projSvc);
    const QVariantMap stats{{QStringLiteral("files"), 2}};

    // 目录不匹配 → 不写
    wf.stampSourceArea(tmp.filePath(QStringLiteral("elsewhere")), stats);
    {
      bool ok = false;
      const PaleoProjectFile pf = readProjectFile(paleoProjectFilePath(projectDir), &ok);
      QVERIFY(ok);
      QVERIFY(pf.sourceAreaRoot.isEmpty());
    }
    // 目录匹配 → 回填
    wf.stampSourceArea(projectDir, stats);
    {
      bool ok = false;
      const PaleoProjectFile pf = readProjectFile(paleoProjectFilePath(projectDir), &ok);
      QVERIFY(ok);
      QCOMPARE(pf.sourceAreaRoot, projectDir);
      QCOMPARE(pf.sourceStats.value(QStringLiteral("files")).toInt(), 2);
    }
  }

// ---- wave/data-foundation T2 ------------------------------------------------

// 任务池在场 → 预览扫描走 worker，done 在 GUI 线程回调、行集与同步路径一致；
// previewActiveChanged 双发。
void previewFolderAsyncOnTaskPool()
  {
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  QVERIFY(!stageFixture(tmp, QStringLiteral("src_area"),
                        QStringLiteral("ExportWellHead.dat")).isEmpty());
  QVERIFY(!stageFixture(tmp, QStringLiteral("src_area"),
                        QStringLiteral("A1.Las")).isEmpty());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack != nullptr);

  PaleoTaskService tasks(stack->store.get());
  FolderImportWorkflow wf(stack->importSvc.get(), &tasks);
  QSignalSpy activeSpy(&wf, &FolderImportWorkflow::previewActiveChanged);

  bool doneCalled = false;
  QVector<FolderPreviewRow> rows;
  QString err;
  wf.previewFolderAsync(
      tmp.filePath(QStringLiteral("src_area")),
      [&doneCalled, &rows, &err](const QVector<FolderPreviewRow> &r, const QString &e) {
        doneCalled = true;
        rows = r;
        err = e;
      });
  QElapsedTimer clock;
  clock.start();
  while (!doneCalled && clock.elapsed() < 15000)
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  QVERIFY(doneCalled);
  QVERIFY2(err.isEmpty(), qPrintable(err));
  QCOMPARE(rows.size(), 2); // 井口 + LAS（与同步路径同一行集）
  QCOMPARE(activeSpy.count(), 2);
  QCOMPARE(activeSpy.at(0).at(0).toBool(), true);
  QCOMPARE(activeSpy.at(1).at(0).toBool(), false);
}

// 「仍导入」改判：重复行默认 Skipped；forceImportPaths 翻成 as_new_version
// 后结局 Imported（同字节结局 = AlreadyStored + 补挂口径）。
void importFolderForceImportFlipsDuplicate()
  {
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString staged =
      stageFixture(tmp, QStringLiteral("src_area"),
                   QStringLiteral("ExportWellHead.dat"));
  QVERIFY(!staged.isEmpty());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack != nullptr);

  FolderImportWorkflow wf(stack->importSvc.get(), nullptr);
  QString err;
  const QVector<FolderRowResult> first =
      wf.importService()->importFolder(tmp.filePath(QStringLiteral("src_area")), &err);
  QVERIFY2(err.isEmpty(), qPrintable(err));
  QCOMPARE(first.size(), 1);
  QCOMPARE(first.at(0).outcome, FolderRowResult::Outcome::Imported);

  const QVector<FolderRowResult> again =
      wf.importService()->importFolder(tmp.filePath(QStringLiteral("src_area")), &err);
  QCOMPARE(again.at(0).outcome, FolderRowResult::Outcome::Skipped);

  const QVector<FolderRowResult> forced = wf.importService()->importFolder(
      tmp.filePath(QStringLiteral("src_area")), &err, QMap<QString, QString>{},
      QStringList{staged});
  QVERIFY2(err.isEmpty(), qPrintable(err));
  QCOMPARE(forced.at(0).outcome, FolderRowResult::Outcome::Imported);
  QVERIFY(forced.at(0).message.contains(QStringLiteral("字节已在库")));
}

// folderconfirm 细化：大小估算行、「仍导入」按钮（仅当壳给了
// importAllWithForced 时出现）、改判集合送达壳出口。
void folderConfirmEstimateForceImportAndErrorReport()
  {
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString staged =
      stageFixture(tmp, QStringLiteral("src_area"),
                   QStringLiteral("ExportWellHead.dat"));
  QVERIFY(!staged.isEmpty());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack != nullptr);
  QString err;
  QVERIFY(!stack->importSvc->importProjectFile(staged, &err).isEmpty());

  const QVector<FolderPreviewRow> preview =
      wfPreview(*stack, tmp.filePath(QStringLiteral("src_area")));
  QCOMPARE(preview.size(), 1);
  QCOMPARE(preview.at(0).decision, QStringLiteral("skip"));
  QVERIFY(preview.at(0).sizeBytes > 0);

  QDialog dlg;
  QStringList gotForced;
  PaleoFolderConfirm::Hooks hooks;
  hooks.importAllWithForced =
      [&gotForced](const QMap<QString, QString> &, const QStringList &forcePaths,
                   const std::function<void(const QVector<FolderRowResult> &,
                                            const QString &)> &) {
        gotForced = forcePaths;
      };
  PaleoFolderConfirm::buildFolderConfirmDialog(
      &dlg, tmp.filePath(QStringLiteral("src_area")), preview, hooks);

  auto *estimate = dlg.findChild<QLabel *>(QStringLiteral("folderEstimateLabel"));
  QVERIFY(estimate != nullptr);
  QVERIFY(!estimate->text().isEmpty());
  QVERIFY(estimate->text().contains(QStringLiteral("重复跳过 1")));

  auto *force = dlg.findChild<QPushButton *>(QStringLiteral("folderForceImport0"));
  QVERIFY(force != nullptr);
  force->click();
  QCOMPARE(force->text(), QStringLiteral("将导入"));

  dlg.findChild<QPushButton *>(QStringLiteral("folderConfirmButton"))->click();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  QCOMPARE(gotForced.size(), 1);
  QCOMPARE(gotForced.at(0), staged);

  auto *errorReport = dlg.findChild<QLabel *>(QStringLiteral("folderErrorReport"));
  QVERIFY(errorReport != nullptr);
  QVERIFY(errorReport->isHidden()); // 尚无失败——错误报告不出现
}
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestFolderImport tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_folderimport.moc"
