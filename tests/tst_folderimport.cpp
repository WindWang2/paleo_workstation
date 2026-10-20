// W7 收口测试：workflow/folderimport 编排契约（docs/UI_LAYER_PLAN.md W7.4）。
// 覆盖：previewFolder 只列不导、importFolder 同步路径 + importActiveChanged
// 双发、importFolderRow 重试、importFile 同步路径、importedWellHeadAsset
// 文件名回查、projectopen 的 stampSourceArea 目录门控。
#include <QtTest>
#include <QSet>
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
#include "../src/workflow/importledger.h"
#include "../src/services/paleotaskservice.h"
#include "../src/ui/dialogs/folderconfirm.h"
#include "perfbudget_relax.h"

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

  // 多选文件导入（文件列表口径）：previewFilesAsync 只列不导；importFiles
  // 同步路径入库、未知类型按 unknown 入辅助口径、importActiveChanged 双发、
  // 台账落首文件所在目录。
  void importFilesSyncPathAndLedger()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString srcDir = tmp.filePath(QStringLiteral("loose"));
    QVERIFY(QDir().mkpath(srcDir));
    QVERIFY(!stageFixture(tmp, QStringLiteral("loose"),
                          QStringLiteral("ExportWellHead.dat")).isEmpty());
    {
      QFile odd(QDir(srcDir).filePath(QStringLiteral("ref.zzz")));
      QVERIFY(odd.open(QIODevice::WriteOnly));
      odd.write("opaque bytes");
      odd.close();
    }

    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    FolderImportWorkflow wf(stack->importSvc.get(), nullptr);
    const QStringList paths{QDir(srcDir).filePath(QStringLiteral("ExportWellHead.dat")),
                            QDir(srcDir).filePath(QStringLiteral("ref.zzz"))};
    // 预览（无任务服务 → 同步回调）：行带分类，纯查询不产资产。
    QVector<FolderPreviewRow> previewRows;
    QString previewErr;
    wf.previewFilesAsync(paths,
                         [&previewRows, &previewErr](const QVector<FolderPreviewRow> &r,
                                                     const QString &e) {
                           previewRows = r;
                           previewErr = e;
                         });
    QVERIFY2(!previewRows.isEmpty(), qPrintable(previewErr));
    QVERIFY(stack->importSvc->catalog()->assets().isEmpty());

    QSignalSpy activeSpy(&wf, &FolderImportWorkflow::importActiveChanged);
    bool doneCalled = false;
    QVector<FolderRowResult> rows;
    QString importErr;
    wf.importFiles(paths, {}, {},
                   [&doneCalled, &rows, &importErr](const QVector<FolderRowResult> &r,
                                                    const QString &e) {
                     doneCalled = true;
                     rows = r;
                     importErr = e;
                   });
    QVERIFY(doneCalled);
    QVERIFY2(importErr.isEmpty(), qPrintable(importErr));
    QCOMPARE(rows.size(), 2);
    int unknowns = 0;
    for (const FolderRowResult &r : rows)
    {
      QCOMPARE(r.outcome, FolderRowResult::Outcome::Imported);
      if (r.classifiedType == QLatin1String("unknown"))
        ++unknowns;
    }
    QCOMPARE(unknowns, 1);
    QCOMPARE(activeSpy.count(), 2);
    QCOMPARE(stack->importSvc->catalog()->assets().size(), 2);
    // 台账：批目录 = 首文件所在目录。
    DataCatalog *cat = stack->importSvc->catalog();
    paleo::imports::ImportLedger ledger;
    ledger.load(cat);
    const auto batches = ledger.batches();
    QCOMPARE(batches.size(), 1);
    QCOMPARE(batches.first().dir, srcDir);
    QCOMPARE(batches.first().imported, 2);
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

  // opt-01 U1：同名井口表分处不同子目录——回查必须按导入源路径定位到
  // 本行自己的资产，不能因文件名相同打开另一份资产的预览（旧实现取
  // 第一份同名资产）。两份内容不同（各自入库、各自资产）。
  void importedWellHeadAssetResolvesSameNameBySourcePath()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString first = stageFixture(tmp, QStringLiteral("src_area/a"),
                                       QStringLiteral("ExportWellHead.dat"));
    const QString second = stageFixture(tmp, QStringLiteral("src_area/b"),
                                        QStringLiteral("ExportWellHead.dat"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());
    {
      // 第二份追加一个空行：同名、解析结果同形，但字节不同（不触发 dedup）。
      QFile f(second);
      QVERIFY(f.open(QIODevice::Append));
      QVERIFY(f.write("\n") == 1);
    }

    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataCatalog *cat = stack->importSvc->catalog();

    QString err;
    const QString idFirst = stack->importSvc->importProjectFile(first, &err);
    QVERIFY2(!idFirst.isEmpty(), qPrintable(err));
    const QString idSecond = stack->importSvc->importProjectFile(second, &err);
    QVERIFY2(!idSecond.isEmpty(), qPrintable(err));
    QVERIFY(idFirst != idSecond);

    FolderImportWorkflow wf(stack->importSvc.get(), nullptr);
    // 真值：两次查询各自回到登记 sourceUri 与行路径一致的资产。
    QCOMPARE(wf.importedWellHeadAsset(first), idFirst);
    QCOMPARE(wf.importedWellHeadAsset(second), idSecond);
    QCOMPARE(QFileInfo(cat->currentVersion(idSecond).sourceUri).absoluteFilePath(),
             QFileInfo(second).absoluteFilePath());
    // 同名但不在库的第三条路径：两份同名候选有歧义 → 不猜（空）。
    const QString third = stageFixture(tmp, QStringLiteral("src_area/c"),
                                       QStringLiteral("ExportWellHead.dat"));
    QVERIFY(!third.isEmpty());
    QCOMPARE(wf.importedWellHeadAsset(third), QString());
#ifndef Q_OS_WIN
    // 符号链接别名（词法不等、规范路径相等）：第一遍词法落空后，第二遍按
    // canonicalFilePath 仍找回正确资产（S5 U1-c：canonical 只在第二遍做）。
    //（Windows 的 QFile::link 生成 .lnk 快捷方式而非符号链接，子例仅 POSIX 覆盖。）
    const QString alias = tmp.filePath(QStringLiteral("alias_area"));
    QVERIFY(QFile::link(tmp.filePath(QStringLiteral("src_area")), alias));
    QCOMPARE(wf.importedWellHeadAsset(alias + QStringLiteral("/b/ExportWellHead.dat")), idSecond);
    QCOMPARE(wf.importedWellHeadAsset(alias + QStringLiteral("/a/ExportWellHead.dat")), idFirst);
#endif
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

  // opt-01 R-9：目录内多个 .qgz 且无同名主工程 → 不静默取字母序第一个，
  // 如实列出候选、不打开任何工程（旧实现直接 openProjectAsync(a.qgz)）。
  void openPathRefusesAmbiguousQgzDirectory()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.filePath(QStringLiteral("area"));
    QVERIFY(QDir().mkpath(dir));
    for (const char *name : {"a_backup.qgz", "b_main.qgz"})
    {
      QFile f(QDir(dir).filePath(QLatin1String(name)));
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("PK");
    }
    QgisProjectService projSvc;
    ProjectOpenWorkflow wf(&projSvc);
    QSignalSpy failed(&wf, &ProjectOpenWorkflow::openFailed);
    QSignalSpy folder(&wf, &ProjectOpenWorkflow::folderImportRequested);
    QVERIFY(!wf.openPath(dir));
    QCOMPARE(failed.count(), 1);
    const QString detail = failed.at(0).at(1).toString();
    QVERIFY2(detail.contains(QStringLiteral("a_backup.qgz")) &&
                 detail.contains(QStringLiteral("b_main.qgz")),
             qPrintable(detail));
    QCOMPARE(failed.at(0).at(2).toBool(), false); // 可解释的提醒，不是致命错误
    QCOMPARE(folder.count(), 0);
    QVERIFY(projSvc.projectPath().isEmpty()); // 没有打开任何工程
    // 候选文件原样不动。
    QCOMPARE(QFileInfo(QDir(dir).filePath(QStringLiteral("a_backup.qgz"))).size(), qint64(2));

    // S5 第 2 轮 Low 2：只数文件（名为 *.qgz 的目录不是候选），清单封顶 20 条。
    const QString many = tmp.filePath(QStringLiteral("many"));
    QVERIFY(QDir().mkpath(QDir(many).filePath(QStringLiteral("folder.qgz"))));
    for (int i = 0; i < 25; ++i)
    {
      QFile f(QDir(many).filePath(QStringLiteral("c%1.qgz").arg(i, 2, 10, QLatin1Char('0'))));
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("PK");
    }
    QSignalSpy failedMany(&wf, &ProjectOpenWorkflow::openFailed);
    QVERIFY(!wf.openPath(many));
    QCOMPARE(failedMany.count(), 1);
    const QString manyDetail = failedMany.at(0).at(1).toString();
    QVERIFY2(manyDetail.contains(QStringLiteral(" 25 个工程文件")), qPrintable(manyDetail));
    QVERIFY(!manyDetail.contains(QStringLiteral("folder.qgz")));
    QVERIFY(manyDetail.contains(QStringLiteral("c19.qgz")));
    QVERIFY(!manyDetail.contains(QStringLiteral("c20.qgz")));
    QVERIFY2(manyDetail.contains(QStringLiteral("另有 5 个未列出")), qPrintable(manyDetail));
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
  while (!doneCalled && clock.elapsed() < paleo::perfbudget::relaxedBudgetMs(15000))
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  QVERIFY(doneCalled);
  QVERIFY2(err.isEmpty(), qPrintable(err));
  QCOMPARE(rows.size(), 2); // 井口 + LAS（与同步路径同一行集）
  QCOMPARE(activeSpy.count(), 2);
  QCOMPARE(activeSpy.at(0).at(0).toBool(), true);
  QCOMPARE(activeSpy.at(1).at(0).toBool(), false);
}

// 审计 02 M-8：任务池在场的异步导入 = worker 只 produce、GUI 提交。多个
// importFolder/importFile 同时发起 → FIFO 串行、全部回调、全部入库；worker
// 从未碰活 catalog（线程违规计数 0）；importActiveChanged 成对、imported
// 信号在对应作业回调之前发出（壳的逐文件标签抑制口径不变）。
void concurrentWorkflowImportsCommitOnGuiThread()
  {
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack != nullptr);
  DataCatalog::resetThreadViolationCount();

  constexpr int kFolders = 4;
  constexpr int kFiles = 6;
  QStringList folders, files;
  const auto lasBytes = [](const QByteArray &well, int n) {
    return QByteArrayLiteral("~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. ") +
           well + QByteArrayLiteral(" : WELL\n~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n") +
           QByteArray::number(100 + n) + QByteArrayLiteral(".0 50.0\n");
  };
  for (int i = 0; i < kFolders; ++i)
  {
    const QString d = tmp.filePath(QStringLiteral("area%1").arg(i));
    QVERIFY(QDir().mkpath(d));
    for (int k = 0; k < 2; ++k)
    {
      QFile f(QDir(d).filePath(QStringLiteral("F%1_%2.las").arg(i).arg(k)));
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write(lasBytes("F" + QByteArray::number(i), i * 10 + k));
    }
    folders.append(d);
  }
  for (int i = 0; i < kFiles; ++i)
  {
    const QString p = tmp.filePath(QStringLiteral("single%1.las").arg(i));
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(lasBytes("S" + QByteArray::number(i), 1000 + i));
    files.append(p);
  }

  PaleoTaskService tasks(stack->store.get());
  FolderImportWorkflow wf(stack->importSvc.get(), &tasks);
  QSignalSpy activeSpy(&wf, &FolderImportWorkflow::importActiveChanged);
  int importedSignals = 0;
  QObject::connect(stack->importSvc.get(), &DataImportService::imported, &wf,
                   [&importedSignals] { ++importedSignals; });
  int doneCount = 0;
  QStringList errors;
  QVector<int> importedAtDone;
  for (const QString &d : folders)
    wf.importFolder(d, {}, [&](const QVector<FolderRowResult> &rows, const QString &e) {
      ++doneCount;
      importedAtDone.append(importedSignals);
      if (!e.isEmpty())
        errors.append(e);
      for (const FolderRowResult &r : rows)
        if (r.outcome == FolderRowResult::Outcome::Failed)
          errors.append(r.message);
    });
  for (const QString &f : files)
    wf.importFile(QStringLiteral("well_log"), f, [&](const QString &id, const QString &e) {
      ++doneCount;
      importedAtDone.append(importedSignals);
      if (id.isEmpty())
        errors.append(e.isEmpty() ? QStringLiteral("empty asset id") : e);
    });
  QVERIFY(wf.pendingImportJobs() >= 1);
  QElapsedTimer clock;
  clock.start();
  while (doneCount < kFolders + kFiles && clock.elapsed() < paleo::perfbudget::relaxedBudgetMs(60000))
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  QCOMPARE(doneCount, kFolders + kFiles);
  QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
  QCOMPARE(wf.pendingImportJobs(), 0);
  QCOMPARE(stack->importSvc->catalog()->assets().size(), kFolders * 2 + kFiles);
  QCOMPARE(importedSignals, kFolders * 2 + kFiles);
  // FIFO：第 n 个作业回调时，此前作业 + 它自己的 imported 已全部发出。
  QCOMPARE(importedAtDone.size(), kFolders + kFiles);
  for (int i = 0; i < kFolders; ++i)
    QCOMPARE(importedAtDone.at(i), (i + 1) * 2);
  for (int i = 0; i < kFiles; ++i)
    QCOMPARE(importedAtDone.at(kFolders + i), kFolders * 2 + i + 1);
  QCOMPARE(activeSpy.count(), 2 * kFolders);
  QCOMPARE(DataCatalog::threadViolationCount(), 0);
  QVERIFY(!QDir(QDir(projectDir).filePath(QStringLiteral("artifacts/staging"))).exists());
  QVERIFY(tasks.shutdown(5000));
}

// 审计 02 M-8：异步导入期间 GUI 侧另有 catalog 写入 → 提交冲突 → 作业用
// 新 session 自动重做，结局正确（不丢 GUI 写入、不重复入库）。
void workflowImportRetriesOnConcurrentCatalogWrite()
  {
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack != nullptr);
  const QString las = tmp.filePath(QStringLiteral("r1.las"));
  {
    QFile f(las);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. R1 : WELL\n"
            "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n");
  }
  PaleoTaskService tasks(stack->store.get());
  FolderImportWorkflow wf(stack->importSvc.get(), &tasks);
  DataCatalog *cat = stack->importSvc->catalog();
  bool done = false;
  QString assetId, err;
  wf.importFile(QStringLiteral("well_log"), las, [&](const QString &id, const QString &e) {
    done = true;
    assetId = id;
    err = e;
  });
  // produce 已在 worker 上排队/在途：GUI 线程同步写一口井（revision 前进）。
  CatalogEntity w;
  w.id = cat->nextEntityId(QStringLiteral("well"));
  w.entityType = QStringLiteral("well");
  w.name = QStringLiteral("GUI-W");
  QString werr;
  QVERIFY2(cat->addEntity(w, &werr), qPrintable(werr));
  QElapsedTimer clock;
  clock.start();
  while (!done && clock.elapsed() < paleo::perfbudget::relaxedBudgetMs(30000))
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  QVERIFY(done);
  QVERIFY2(!assetId.isEmpty(), qPrintable(err));
  QVERIFY(cat->hasEntity(w.id));
  QCOMPARE(cat->assets().size(), 1);
  QVERIFY(tasks.shutdown(5000));
}

// opt-01 U3：文件夹导入作业在飞时导入服务先析构（壳收尾/换工程）——
// finished 处理器必须看到「服务已不在」：不经悬垂指针写台账、不把未提交
// 的行报成「已入库」，回调如实收到丢弃原因；源文件字节不受影响。
void folderImportSurvivesServiceTeardownHonestly()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack != nullptr);
  const QString area = tmp.filePath(QStringLiteral("area"));
  QVERIFY(QDir().mkpath(area));
  const QByteArray lasBytes =
      "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. T1 : WELL\n"
      "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n";
  const QString las = QDir(area).filePath(QStringLiteral("T1.las"));
  {
    QFile f(las);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(lasBytes), qint64(lasBytes.size()));
  }
  PaleoTaskService tasks(stack->store.get());
  FolderImportWorkflow wf(stack->importSvc.get(), &tasks);
  bool done = false;
  QVector<FolderRowResult> rows;
  QString err;
  wf.importFolder(area, {}, [&](const QVector<FolderRowResult> &r, const QString &e) {
    done = true;
    rows = r;
    err = e;
  });
  // 作业已登记、finished 只能在事件泵里跑：此刻析构服务。
  stack->importSvc.reset();
  QCOMPARE(wf.importService(), static_cast<DataImportService *>(nullptr));
  QElapsedTimer clock;
  clock.start();
  while (!done && clock.elapsed() < paleo::perfbudget::relaxedBudgetMs(30000))
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  QVERIFY(done);
  QCOMPARE(err, QStringLiteral("导入服务已关闭，导入结果已丢弃"));
  // 行不能为空（否则下方逐行断言空转）：produce 已在 staging 上产出该行，
  // 服务不在场时如实改报失败并带丢弃原因。
  QVERIFY(!rows.isEmpty());
  for (const FolderRowResult &r : rows)
  {
    QVERIFY2(r.outcome == FolderRowResult::Outcome::Failed,
             qPrintable(r.path + QStringLiteral(" 未提交却未报失败")));
    QCOMPARE(r.message, err);
  }
  QCOMPARE(wf.pendingImportJobs(), 0);
  // 原始资料不受损：源文件字节原样。
  QFile src(las);
  QVERIFY(src.open(QIODevice::ReadOnly));
  QCOMPARE(src.readAll(), lasBytes);
  // 服务不在场时再发起导入：importFolder 入口的 !m_svc 守卫直接走失败回调
  //（空行 + 原因），不建作业、不触悬垂指针。
  bool failed = false;
  wf.importFolder(area, {}, [&](const QVector<FolderRowResult> &r, const QString &e) {
    failed = r.isEmpty() && !e.isEmpty();
  });
  QVERIFY(failed);
  QVERIFY(tasks.shutdown(5000));
}

// opt-01 P5：文件夹导入在飞时切换工程——整批作废（「工程已切换」），且不得把
// 这批记进新工程的导入台账（旧实现按当前 catalog 推台账路径，串账到新工程）。
// 新工程资产数不变、源文件字节不变。
void folderImportAcrossProjectSwitchDoesNotCrossPostLedger()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString projectA = tmp.filePath(QStringLiteral("projA"));
  const QString projectB = tmp.filePath(QStringLiteral("projB"));
  QVERIFY(QDir().mkpath(projectA) && QDir().mkpath(projectB));
  auto stack = makeStack(projectA);
  QVERIFY(stack != nullptr);
  const QString area = tmp.filePath(QStringLiteral("area"));
  QVERIFY(QDir().mkpath(area));
  const QByteArray lasBytes =
      "~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. S1 : WELL\n"
      "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n";
  const QString las = QDir(area).filePath(QStringLiteral("S1.las"));
  {
    QFile f(las);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(lasBytes), qint64(lasBytes.size()));
  }
  PaleoTaskService tasks(stack->store.get());
  FolderImportWorkflow wf(stack->importSvc.get(), &tasks);
  bool done = false;
  QVector<FolderRowResult> rows;
  QString err;
  wf.importFolder(area, {}, [&](const QVector<FolderRowResult> &r, const QString &e) {
    done = true;
    rows = r;
    err = e;
  });
  stack->importSvc->setProjectDir(projectB); // produce 在途：切到工程 B
  DataCatalog *catB = stack->importSvc->catalog();
  QVERIFY(catB && catB->isOpen());
  const int assetsBefore = catB->assets().size();
  QElapsedTimer clock;
  clock.start();
  while (!done && clock.elapsed() < paleo::perfbudget::relaxedBudgetMs(30000))
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  QVERIFY(done);
  QVERIFY2(err.contains(QStringLiteral("工程已切换")), qPrintable(err));
  QVERIFY(!rows.isEmpty());
  for (const FolderRowResult &r : rows)
    QCOMPARE(r.outcome, FolderRowResult::Outcome::Failed);
  QCOMPARE(catB->assets().size(), assetsBefore);
  paleo::imports::ImportLedger ledgerB;
  ledgerB.load(catB);
  QVERIFY(!ledgerB.filePath().isEmpty());
  QVERIFY2(QFileInfo(ledgerB.filePath()).absoluteFilePath().startsWith(QDir(projectB).absolutePath()),
           qPrintable(ledgerB.filePath()));
  QCOMPARE(ledgerB.count(), 0); // 没有串账
  QFile src(las);
  QVERIFY(src.open(QIODevice::ReadOnly));
  QCOMPARE(src.readAll(), lasBytes);
  // 同工程的正常批次仍入账（守护：判定不误伤）——以另一种写法（冗余 ./ 与
  // 尾部斜杠）重设同一工程，归一化后仍判同一工程、照常入账。
  stack->importSvc->setProjectDir(projectB + QStringLiteral("/./"));
  catB = stack->importSvc->catalog();
  QVERIFY(catB && catB->isOpen());
  done = false;
  wf.importFolder(area, {}, [&](const QVector<FolderRowResult> &r, const QString &e) {
    done = true;
    rows = r;
    err = e;
  });
  clock.restart();
  while (!done && clock.elapsed() < paleo::perfbudget::relaxedBudgetMs(30000))
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  QVERIFY(done);
  ledgerB.load(catB);
  QCOMPARE(ledgerB.count(), 1);
  QCOMPARE(ledgerB.batches()[0].rows.size(), rows.size());
#ifndef Q_OS_WIN
  // S5 第 2 轮 Low 14：预开 catalog 经符号链接别名打开（catalog 目录 = 别名，
  // session.projectDir = 真路径）——词法不等、规范路径相等，仍判同一工程入账。
  const QString projectC = tmp.filePath(QStringLiteral("projC"));
  const QString aliasC = tmp.filePath(QStringLiteral("projC_alias"));
  QVERIFY(QDir().mkpath(projectC));
  QVERIFY(QFile::link(projectC, aliasC));
  auto prepared = DataCatalog::prepareOpen(aliasC, false);
  QVERIFY(prepared && prepared->isOpen());
  stack->importSvc->setProjectDir(projectC, prepared);
  DataCatalog *catC = stack->importSvc->catalog();
  QVERIFY2(catC && catC->isOpen(), qPrintable(stack->importSvc->catalogOpenError()));
  QVERIFY(QDir::cleanPath(catC->catalogPath()).startsWith(QDir::cleanPath(aliasC)));
  done = false;
  wf.importFolder(area, {}, [&](const QVector<FolderRowResult> &r, const QString &e) {
    done = true;
    rows = r;
    err = e;
  });
  clock.restart();
  while (!done && clock.elapsed() < paleo::perfbudget::relaxedBudgetMs(30000))
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  QVERIFY(done);
  paleo::imports::ImportLedger ledgerC;
  ledgerC.load(catC);
  QCOMPARE(ledgerC.count(), 1);
#endif
  QVERIFY(tasks.shutdown(5000));
}

// A2（方向 01）：单文件导入作业取消后不得提交——对齐 dataops 队列
// 「取消的作业不提交」语义（session 析构删暂存）。produce 对取消无检查点
//（整文件原子完成），用户叫停后把成品悄悄入库违背取消承诺。
void batchTerminalGuardDiscardsProducedCatalog_data()
{
  QTest::addColumn<bool>("cancel");
  QTest::newRow("cancel-after-worker-before-commit") << true;
  QTest::newRow("failed-after-produce-before-commit") << false;
}

void batchTerminalGuardDiscardsProducedCatalog()
{
  QFETCH(bool, cancel);
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString projectDir = temporary.filePath(QStringLiteral("project"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack);
  const QString head = stageFixture(temporary, QStringLiteral("source"),
                                    QStringLiteral("ExportWellHead.dat"));
  const QString log = stageFixture(temporary, QStringLiteral("source"),
                                   QStringLiteral("A1.Las"));
  QVERIFY(!head.isEmpty() && !log.isEmpty());
  PaleoTaskService tasks(stack->store.get());
  bool produced = false;
  PaleoTask *finishedTask = nullptr;
  // taskAdded 在 workflow 接 finished 之前发出：精确卡住 worker 完成后的提交窄窗。
  connect(&tasks, &PaleoTaskService::taskAdded, &tasks, [&](PaleoTask *task) {
    connect(task, &PaleoTask::finished, &tasks, [&, task] {
      produced = task->state() == PaleoTask::State::Succeeded;
      finishedTask = task;
      if (cancel) task->requestCancel();
      else task->failPublication(QStringLiteral("模拟 worker 失败，禁止提交"));
    });
  });
  FolderImportWorkflow workflow(stack->importSvc.get(), &tasks);
  bool done = false;
  QString error;
  QVector<FolderRowResult> results;
  workflow.importFiles({head, log}, {}, {}, [&](const auto &rows, const QString &message) {
    results = rows;
    error = message;
    done = true;
  });
  QTRY_VERIFY_WITH_TIMEOUT(done, paleo::perfbudget::relaxedBudgetMs(30000));
  QVERIFY(produced);
  QVERIFY(finishedTask);
  QVERIFY(!error.isEmpty());
  if (cancel) {
    QVERIFY(error.contains(QStringLiteral("已取消")));
    QCOMPARE(finishedTask->state(), PaleoTask::State::Cancelled);
  } else {
    QVERIFY(error.contains(QStringLiteral("模拟 worker 失败")));
    QCOMPARE(finishedTask->state(), PaleoTask::State::Failed);
  }
  QVERIFY(!results.isEmpty());
  for (const auto &row : results) {
    QVERIFY(row.outcome != FolderRowResult::Outcome::Imported);
    QVERIFY(row.outcome != FolderRowResult::Outcome::Unresolved);
  }
  QVERIFY(stack->importSvc->catalog()->assets().isEmpty());
  QVERIFY(stack->importSvc->catalog()->entities(QStringLiteral("well")).isEmpty());
  QVERIFY(!QDir(QDir(projectDir).filePath(QStringLiteral("artifacts/staging"))).exists());
  QVERIFY(tasks.shutdown(5000));
}

void batchCancellationHandleGuardsCommitAndKeepsSameSourceJob()
{
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString projectDir = temporary.filePath(QStringLiteral("project"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack);
  const QString head = stageFixture(temporary, QStringLiteral("source"),
                                    QStringLiteral("ExportWellHead.dat"));
  const QString log = stageFixture(temporary, QStringLiteral("source"),
                                   QStringLiteral("A1.Las"));
  QVERIFY(!head.isEmpty() && !log.isEmpty());
  PaleoTaskService tasks(stack->store.get());
  FolderImportWorkflow workflow(stack->importSvc.get(), &tasks);
  FolderImportWorkflow::CancelImport cancelFirst;
  int taskCount = 0;
  bool produced = false;
  connect(&tasks, &PaleoTaskService::taskAdded, &tasks, [&](PaleoTask *task) {
    if (++taskCount != 1) return;
    connect(task, &PaleoTask::finished, &tasks, [&, task] {
      produced = task->state() == PaleoTask::State::Succeeded;
      if (cancelFirst) cancelFirst();
    });
  });
  int completions = 0;
  QString firstError;
  cancelFirst = workflow.importFiles({head}, {}, {}, [&](const auto &, const auto &error) {
    firstError = error;
    QVERIFY(stack->importSvc->catalog()->assets().isEmpty());
    ++completions;
  });
  auto cancelSecond = workflow.importFiles({head}, {}, {}, [&](const auto &rows, const auto &error) {
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!rows.isEmpty());
    QCOMPARE(rows.first().outcome, FolderRowResult::Outcome::Imported);
    cancelFirst(); // 已完成句柄不得取消下一批（来源可以完全相同）。
    ++completions;
  });
  workflow.importFiles({log}, {}, {}, [&](const auto &rows, const auto &error) {
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!rows.isEmpty());
    QCOMPARE(rows.first().outcome, FolderRowResult::Outcome::Imported);
    ++completions;
  });
  QVERIFY(cancelFirst && cancelSecond);
  QTRY_COMPARE_WITH_TIMEOUT(completions, 3, paleo::perfbudget::relaxedBudgetMs(30000));
  QVERIFY(produced);
  QVERIFY(firstError.contains(QStringLiteral("已取消")));
  QCOMPARE(stack->importSvc->catalog()->assets().size(), 2);
  QCOMPARE(workflow.pendingImportJobs(), 0);
  QVERIFY(tasks.shutdown(5000));
}

void queuedBatchCancellationHandleNeverStartsWorker()
{
  QTemporaryDir temporary;
  QVERIFY(temporary.isValid());
  const QString projectDir = temporary.filePath(QStringLiteral("project"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack);
  const QString head = stageFixture(temporary, QStringLiteral("source"),
                                    QStringLiteral("ExportWellHead.dat"));
  const QString log = stageFixture(temporary, QStringLiteral("source"),
                                   QStringLiteral("A1.Las"));
  QVERIFY(!head.isEmpty() && !log.isEmpty());
  PaleoTaskService tasks(stack->store.get());
  FolderImportWorkflow workflow(stack->importSvc.get(), &tasks);
  QSignalSpy added(&tasks, &PaleoTaskService::taskAdded);
  int completions = 0;
  workflow.importFolder(QFileInfo(head).absolutePath(), {}, [&](const auto &, const auto &error) {
    QVERIFY2(error.isEmpty(), qPrintable(error));
    ++completions;
  });
  QString cancelledError;
  const auto cancel = workflow.importFiles({log}, {}, {}, [&](const auto &rows, const auto &error) {
    QVERIFY(rows.isEmpty());
    cancelledError = error;
    ++completions;
  });
  QVERIFY(cancel);
  cancel();
  QTRY_COMPARE_WITH_TIMEOUT(completions, 2, paleo::perfbudget::relaxedBudgetMs(30000));
  QVERIFY(cancelledError.contains(QStringLiteral("已取消")));
  QCOMPARE(added.count(), 1);
  QCOMPARE(stack->importSvc->catalog()->assets().size(), 2);
  QCOMPARE(workflow.pendingImportJobs(), 0);
  QVERIFY(tasks.shutdown(5000));
}

void singleFileImportCancelledDiscardsCommit()
{
  QTemporaryDir tmp;
  QVERIFY(tmp.isValid());
  const QString projectDir = tmp.filePath(QStringLiteral("proj"));
  QVERIFY(QDir().mkpath(projectDir));
  auto stack = makeStack(projectDir);
  QVERIFY(stack != nullptr);
  const QString las = tmp.filePath(QStringLiteral("r1.las"));
  {
    QFile f(las);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("~Version Information\nVERS. 2.0:\nWRAP. NO:\n~Well\nWELL. R1 : WELL\n"
            "~Curve\nDEPT.M :\nGR.API :\n~A DEPT GR\n100.0 50.0\n");
  }
  PaleoTaskService tasks(stack->store.get());
  FolderImportWorkflow wf(stack->importSvc.get(), &tasks);
  bool done = false;
  QString assetId, err;
  wf.importFile(QStringLiteral("well_log"), las, [&](const QString &id, const QString &e) {
    done = true;
    assetId = id;
    err = e;
  });
  // importFile 返回时任务已同步登记；finished 处理器只能在事件泵里跑——
  // 此刻同步 requestCancel，处理器必然观察到 cancelRequested()（原子位）。
  for (PaleoTask *t : tasks.tasks())
    t->requestCancel();
  QElapsedTimer clock;
  clock.start();
  while (!done && clock.elapsed() < paleo::perfbudget::relaxedBudgetMs(30000))
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
  QVERIFY(done);
  QVERIFY(assetId.isEmpty());
  QVERIFY2(err.contains(QStringLiteral("已取消")), qPrintable(err));
  QCOMPARE(stack->importSvc->catalog()->assets().size(), 0);
  // 暂存根已随 session 析构清理，无半截残留。
  QVERIFY(!QDir(QDir(projectDir).filePath(QStringLiteral("artifacts/staging"))).exists());
  QCOMPARE(DataCatalog::threadViolationCount(), 0);
  QVERIFY(tasks.shutdown(5000));
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

  // ---- 方向 30：确认表归位预览列（plan 期身份匹配预显）----
  void previewEntityPreviewShowsPlacement()
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
    const QString dir = tmp.filePath(QStringLiteral("src_area"));

    const auto rowFor = [](const QVector<FolderPreviewRow> &rows, const QString &name) {
      for (const FolderPreviewRow &r : rows)
        if (QFileInfo(r.path).fileName() == name)
          return r;
      return FolderPreviewRow();
    };

    // 新工程：井口行预显「新井」；LAS 明确内容井名预显可新建的 A1。
    QVector<FolderPreviewRow> rows = wf.previewFolder(dir);
    QCOMPARE(rowFor(rows, QStringLiteral("ExportWellHead.dat")).entityPreview,
             QStringLiteral("新井"));
    const QString lasPreview = rowFor(rows, QStringLiteral("A1.Las")).entityPreview;
    QCOMPARE(lasPreview, QStringLiteral("新井 A1（来自文件内容）"));
    QVERIFY(!rowFor(rows, QStringLiteral("A1.Las")).needsWellChoice);

    bool done = false;
    wf.importFolder(dir, {}, [&done](const QVector<FolderRowResult> &, const QString &) {
      done = true;
    });
    QVERIFY(done);

    // 井已建齐：井口行预显「井 A1, A2, …（既有）」（井口文件含 20 口井）；
    // LAS 命中既有井 A1。
    rows = wf.previewFolder(dir);
    const QString headPreview =
        rowFor(rows, QStringLiteral("ExportWellHead.dat")).entityPreview;
    QVERIFY2(headPreview.startsWith(QStringLiteral("井 ")) &&
                 headPreview.endsWith(QStringLiteral("（既有）")) &&
                 headPreview.contains(QStringLiteral("A1")),
             qPrintable(headPreview));
    QCOMPARE(rowFor(rows, QStringLiteral("A1.Las")).entityPreview, QStringLiteral("井 A1"));
  }

  // ---- 方向 30：好坏混合导入零静默 + 台账落盘（Oracle 1）----
  void mixedGoodBadImportIsHonestAndLedgered()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVERIFY(!stageFixture(tmp, QStringLiteral("src_area"),
                          QStringLiteral("ExportWellHead.dat")).isEmpty());
    QVERIFY(!stageFixture(tmp, QStringLiteral("src_area"),
                          QStringLiteral("A1.Las")).isEmpty());
    // 坏文件一：读不了（权限）——拷贝阶段即失败，行结局 Failed + 原因。
    // Windows 权限位无 unreadable 语义（readonly 属性不拦读、setPermissions
    // 只认写位），该场景仅 POSIX 侧覆盖；Windows 由 broken.Las 覆盖解析失败。
#ifndef Q_OS_WIN
    const QString unreadable =
        stageFixture(tmp, QStringLiteral("src_area"), QStringLiteral("A1.Las"),
                     QStringLiteral("unreadable.Las"));
    QVERIFY(!unreadable.isEmpty());
    QVERIFY(QFile::setPermissions(unreadable, QFile::Permissions{})); // 零权限——不可读
    QVERIFY(!QFile(unreadable).open(QIODevice::ReadOnly)); // 不可读已生效
#endif
    // 坏文件二：损坏 LAS——原位报失败与原因，不登记无效资产。
    const QString broken = QDir(tmp.filePath(QStringLiteral("src_area")))
                               .filePath(QStringLiteral("broken.Las"));
    {
      QFile f(broken);
      QVERIFY(f.open(QIODevice::WriteOnly));
      f.write("this is not a las file\nno tilde sections\ngarbage bytes \x01\x02");
    }

    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    FolderImportWorkflow wf(stack->importSvc.get(), nullptr);

    QVector<FolderRowResult> rows;
    QString importErr;
    bool done = false;
    wf.importFolder(tmp.filePath(QStringLiteral("src_area")), {},
                    [&](const QVector<FolderRowResult> &r, const QString &e) {
                      rows = r;
                      importErr = e;
                      done = true;
                    });
    QVERIFY(done);
    QVERIFY2(importErr.isEmpty(), qPrintable(importErr));

    // 每个源文件恰好一行（零静默：无文件凭空消失）。
    QSet<QString> seen;
    for (const FolderRowResult &r : rows)
      seen.insert(QFileInfo(r.path).fileName());
    QVERIFY(seen.contains(QStringLiteral("ExportWellHead.dat")));
    QVERIFY(seen.contains(QStringLiteral("A1.Las")));
#ifndef Q_OS_WIN
    QVERIFY(seen.contains(QStringLiteral("unreadable.Las")));
#endif
    QVERIFY(seen.contains(QStringLiteral("broken.Las")));
    QCOMPARE(seen.size(), seen.contains(QStringLiteral("unreadable.Las")) ? 4 : 3);

    using Outcome = FolderRowResult::Outcome;
    Outcome headOutcome = Outcome::Skipped, lasOutcome = Outcome::Skipped,
            unreadableOutcome = Outcome::Skipped, brokenOutcome = Outcome::Skipped;
    QString unreadableMsg, brokenMsg;
    for (const FolderRowResult &r : rows)
    {
      const QString name = QFileInfo(r.path).fileName();
      if (name == QStringLiteral("ExportWellHead.dat"))
        headOutcome = r.outcome;
      else if (name == QStringLiteral("A1.Las"))
        lasOutcome = r.outcome;
      else if (name == QStringLiteral("unreadable.Las"))
      {
        unreadableOutcome = r.outcome;
        unreadableMsg = r.message;
      }
      else if (name == QStringLiteral("broken.Las"))
      {
        brokenOutcome = r.outcome;
        brokenMsg = r.message;
      }
    }
    QCOMPARE(headOutcome, Outcome::Imported);
    QCOMPARE(lasOutcome, Outcome::Imported);
    if (seen.contains(QStringLiteral("unreadable.Las")))
    {
      QCOMPARE(unreadableOutcome, Outcome::Failed);
      QVERIFY(!unreadableMsg.isEmpty()); // 坏文件逐条原因列报
    }
    QCOMPARE(brokenOutcome, Outcome::Failed);
    QVERIFY(!brokenMsg.isEmpty());

    // 好文件全入库：catalog 里有井口与 LAS 资产，井 A1 已建。
    DataCatalog *cat = stack->importSvc->catalog();
    int wellHeads = 0, wellLogs = 0;
    for (const CatalogAsset &a : cat->assets())
    {
      if (a.type == QLatin1String("well_head"))
        ++wellHeads;
      if (a.type == QLatin1String("well_log"))
        ++wellLogs;
    }
    QCOMPARE(wellHeads, 1);
    QCOMPARE(wellLogs, 1); // 仅 A1.Las；损坏与不可读文件不留资产。
    for (const auto &asset : cat->assets())
      QVERIFY(asset.displayName != QStringLiteral("broken.Las"));
    QVERIFY(!cat->wellsMatchingName(QStringLiteral("A1")).isEmpty());

    // 台账：一批落盘，四计数与行集和行结果一致。
    paleo::imports::ImportLedger ledger;
    ledger.load(cat);
    QCOMPARE(ledger.count(), 1);
    const paleo::imports::LedgerBatch batch = ledger.batches().first();
    QCOMPARE(batch.error, QString());
    QCOMPARE(batch.rows.size(), rows.size());
    QCOMPARE(batch.imported + batch.unresolved + batch.failed + batch.skipped,
             rows.size());
    int ledgerFailed = 0;
    for (const paleo::imports::LedgerRow &lr : batch.rows)
    {
      if (lr.outcome == QLatin1String("failed"))
      {
        ++ledgerFailed;
        QVERIFY(!lr.message.isEmpty());
      }
    }
    QCOMPARE(ledgerFailed, seen.contains(QStringLiteral("unreadable.Las")) ? 2 : 1);
    QVERIFY(QFile::exists(QDir(projectDir).filePath(
        QStringLiteral(".paleo/import_ledger.json"))));

    // 再导一批（重复决策）→ 台账两批，窗口滚动。
    bool done2 = false;
    wf.importFolder(tmp.filePath(QStringLiteral("src_area")), {},
                    [&done2](const QVector<FolderRowResult> &, const QString &) {
                      done2 = true;
                    });
    QVERIFY(done2);
    paleo::imports::ImportLedger ledger2;
    ledger2.load(cat);
    QCOMPARE(ledger2.count(), 2);
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
