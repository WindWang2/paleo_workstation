// wave/data-foundation T10：IngestPlan 幂等压力性质测试（重复执行/中断续跑/
// 部分失败）+ T2 快照等价性与 worker 线程无 marshal 构建。
// 性质口径（docs/DATA_FABRIC_ADOPTION.md C 包）：
//   · 幂等——同一目录重复执行每行 Skipped、目录零增量（revision 不涨）；
//   · 中断续跑——执行段取消保留已处理行，重跑收敛到与一次跑完相同的状态；
//   · 部分失败——单行失败只落行不传染，修复后重跑补齐；
//   · 快照——CatalogReadSnapshot 构建的 plan 与活对象逐项一致；worker 线程
//     经快照构建不再 BlockingQueuedConnection 依赖 GUI 事件循环。
#include <QtTest>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThreadPool>

#include <future>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "../src/catalog/datacatalog.h"
#include "../src/io/dataimportservice.h"
#include "../src/io/ingestplan.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"

class TestIngestPlan : public QObject
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

  static QString fixture(const QString &name)
  {
    return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
  }

  static bool writeFile(const QString &path, const QByteArray &content)
  {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return false;
    return f.write(content) == content.size();
  }

  static std::unique_ptr<Stack> makeStack(const QString &projectDir)
  {
    auto s = std::make_unique<Stack>();
    const QString metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
    if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
      return nullptr;
    s->manifest = std::make_unique<LayerManifest>(metaPath);
    QString err;
    if (!s->manifest->open(&err))
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

  // 混合 fixture：井口（2 井）+ 单井 LAS + 时深——覆盖两阶段序与多角色。
  static QString makeArea(const QTemporaryDir &tmp)
  {
    const QString root = tmp.filePath(QStringLiteral("area"));
    if (!QDir().mkpath(QDir(root).filePath(QString::fromUtf8("井位"))))
      return QString();
    if (!QDir().mkpath(QDir(root).filePath(QString::fromUtf8("时深"))))
      return QString();
    if (!writeFile(QDir(root).filePath(QString::fromUtf8("井位/heads.dat")),
                   QByteArrayLiteral("#WellHead File From SMI\n"
                                     "#Name      X     Y     KB    TotalDepth\n"
                                     "A1           1.0   2.0   0.0   2000.0\n"
                                     "B2           3.0   4.0   0.0   2100.0\n")))
      return QString();
    if (!QFile::copy(fixture(QStringLiteral("A1.Las")),
                     QDir(root).filePath(QStringLiteral("0A1.Las"))))
      return QString();
    if (!QFile::copy(fixture(QStringLiteral("A1_TD.dat")),
                     QDir(root).filePath(QString::fromUtf8("时深/A1_TD.dat"))))
      return QString();
    return root;
  }

  static int countBy(const QVector<FolderRowResult> &rows, FolderRowResult::Outcome o)
  {
    int n = 0;
    for (const auto &r : rows)
      if (r.outcome == o)
        ++n;
    return n;
  }

private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  // ---- 幂等：重复执行零增量 ------------------------------------------------
  void repeatRunIsIdempotent()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = makeArea(tmp);
    QVERIFY(!root.isEmpty());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;
    DataCatalog *cat = svc.catalog();

    QString err;
    const QVector<FolderRowResult> first = svc.importFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(first.size(), 3);
    QCOMPARE(countBy(first, FolderRowResult::Outcome::Imported), 3);

    const int assetsBefore = cat->assets().size();
    const int versionsBefore = cat->versions().size();
    const int linksBefore = cat->links().size();
    const int revisionBefore = cat->catalogRevision();

    const QVector<FolderRowResult> second = svc.importFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(second.size(), 3);
    QCOMPARE(countBy(second, FolderRowResult::Outcome::Skipped), 3);
    // 零增量：资产/版本/链接不变；跳过行不触发落盘（revision 不涨）。
    QCOMPARE(cat->assets().size(), assetsBefore);
    QCOMPARE(cat->versions().size(), versionsBefore);
    QCOMPARE(cat->links().size(), linksBefore);
    QCOMPARE(cat->catalogRevision(), revisionBefore);

    // 第三跑再确认稳定（幂等不是只幂一次）。
    const QVector<FolderRowResult> third = svc.importFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(countBy(third, FolderRowResult::Outcome::Skipped), 3);
    QCOMPARE(cat->catalogRevision(), revisionBefore);
  }

  // ---- 中断续跑：取消后重跑收敛到一次跑完的状态 ----------------------------
  void interruptResumeConverges()
  {
    QTemporaryDir full, part;
    QVERIFY(full.isValid() && part.isValid());

    // 参照：一次跑完。
    const QString rootFull = makeArea(full);
    const QString projFull = full.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projFull));
    auto stackFull = makeStack(projFull);
    QVERIFY(stackFull != nullptr);
    QString err;
    stackFull->importSvc->importFolder(rootFull, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    const int refAssets = stackFull->importSvc->catalog()->assets().size();
    const int refVersions = stackFull->importSvc->catalog()->versions().size();
    const int refLinks = stackFull->importSvc->catalog()->links().size();
    const int refWells =
        stackFull->importSvc->catalog()->entities(QStringLiteral("well")).size();
    QCOMPARE(refAssets, 3);

    // 中断：首行执行后取消——已处理行保留。
    const QString rootPart = makeArea(part);
    const QString projPart = part.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projPart));
    auto stackPart = makeStack(projPart);
    QVERIFY(stackPart != nullptr);
    int seen = 0;
    const QVector<FolderRowResult> cut =
        stackPart->importSvc->importFolder(rootPart, &err, QMap<QString, QString>{},
            [&](int, int total, const QString &) {
              if (total == 0)
                return true; // 扫描段（哈希期）放行
              return seen++ < 0; // 执行段：首行回调即取消（行已处理，保留）
            });
    QVERIFY(err.contains(QStringLiteral("已取消")));
    QCOMPARE(cut.size(), 1); // 取消时已处理的行照常在结果里
    QCOMPARE(countBy(cut, FolderRowResult::Outcome::Imported), 1);
    QVERIFY(stackPart->importSvc->catalog()->assets().size() >= 1);

    // 续跑：同目录全量重跑——收敛到与参照完全相同的目录状态。
    const QVector<FolderRowResult> resume =
        stackPart->importSvc->importFolder(rootPart, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(countBy(resume, FolderRowResult::Outcome::Failed), 0);
    DataCatalog *cat = stackPart->importSvc->catalog();
    QCOMPARE(cat->assets().size(), refAssets);
    QCOMPARE(cat->versions().size(), refVersions);
    QCOMPARE(cat->links().size(), refLinks);
    QCOMPARE(cat->entities(QStringLiteral("well")).size(), refWells);
  }

  // ---- 部分失败：单行失败不传染，修复后重跑补齐 ----------------------------
  void partialFailureIsolatedAndRetryRecovers()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = makeArea(tmp);
    QVERIFY(!root.isEmpty());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataImportService &svc = *stack->importSvc;

    // 拿走 LAS 的可读性 → 该行 plan 哈希失败、执行失败；井口/时深不受
    // 传染。POSIX：清空权限位（root 用户不受权限约束的宿主上该用例会
    // 退化——CI 以普通用户跑）。Windows：只读属性不挡读，改持零共享
    // 句柄模拟「占用中不可读」——枚举照常、QFile::open 必败。
    const QString lasPath = QDir(root).filePath(QStringLiteral("0A1.Las"));
#ifdef Q_OS_WIN
    HANDLE lasLock = CreateFileW(
        reinterpret_cast<const wchar_t *>(lasPath.utf16()), GENERIC_READ, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(lasLock != INVALID_HANDLE_VALUE);
#else
    QVERIFY(QFile::setPermissions(lasPath, QFileDevice::Permissions{}));
#endif
    QString err;
    const QVector<FolderRowResult> rows = svc.importFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err)); // 行失败不是整体失败
    QCOMPARE(rows.size(), 3);
    QCOMPARE(countBy(rows, FolderRowResult::Outcome::Failed), 1);
    QCOMPARE(countBy(rows, FolderRowResult::Outcome::Imported), 2);
    QCOMPARE(svc.catalog()->assets().size(), 2);

    // 修复可读性 → 重跑补齐，目录状态 == 一次全绿跑。
#ifdef Q_OS_WIN
    CloseHandle(lasLock);
#else
    QVERIFY(QFile::setPermissions(lasPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                               QFileDevice::ReadUser));
#endif
    const QVector<FolderRowResult> retry = svc.importFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(countBy(retry, FolderRowResult::Outcome::Failed), 0);
    QCOMPARE(svc.catalog()->assets().size(), 3);
    QCOMPARE(svc.catalog()->entities(QStringLiteral("well")).size(), 2);
  }

  // ---- 快照等价：活对象 plan 与快照 plan 逐项一致 --------------------------
  void snapshotPlanEqualsLivePlan()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = makeArea(tmp);
    QVERIFY(!root.isEmpty());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    // 先导入一次，让 plan 面对非空 catalog（去重/身份匹配/主关联占用全激活）。
    QString err;
    stack->importSvc->importFolder(root, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));

    const DataCatalog *cat = stack->importSvc->catalog();
    const IngestPlan live = buildIngestPlan(root, *cat);
    const CatalogReadSnapshot snap = CatalogReadSnapshot::fromCatalog(*cat);
    const IngestPlan fromSnap = buildIngestPlan(root, snap);

    QCOMPARE(live.items.size(), fromSnap.items.size());
    QCOMPARE(live.skipped.size(), fromSnap.skipped.size());
    for (int i = 0; i < live.items.size(); ++i)
    {
      QCOMPARE(live.items.at(i).path, fromSnap.items.at(i).path);
      QCOMPARE(live.items.at(i).type, fromSnap.items.at(i).type);
      QCOMPARE(live.items.at(i).decision, fromSnap.items.at(i).decision);
      QCOMPARE(live.items.at(i).sha256, fromSnap.items.at(i).sha256);
      QCOMPARE(live.items.at(i).entityId, fromSnap.items.at(i).entityId);
      QCOMPARE(live.items.at(i).entityAmbiguous, fromSnap.items.at(i).entityAmbiguous);
      QCOMPARE(live.items.at(i).duplicateOfVersionId,
               fromSnap.items.at(i).duplicateOfVersionId);
      QCOMPARE(live.items.at(i).suggestedPrimary, fromSnap.items.at(i).suggestedPrimary);
    }
  }

  // ---- worker 线程无 marshal 构建：GUI 事件循环不泵也能完成 plan ------------
  void planFromWorkerThreadNeedsNoGuiEventLoop()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = makeArea(tmp);
    QVERIFY(!root.isEmpty());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);
    DataCatalog *cat = stack->importSvc->catalog();
    QString err;
    stack->importSvc->importFolder(root, &err); // 非空 catalog（sha 去重命中路径）
    QVERIFY2(err.isEmpty(), qPrintable(err));

    // 快照在 catalog 线程（= 本测试线程）拷出——这正是 DataImportService::
    // planFor 的顺序。之后 worker 只碰快照。
    const CatalogReadSnapshot snap = CatalogReadSnapshot::fromCatalog(*cat);

    QElapsedTimer clock;
    clock.start();
    std::promise<IngestPlan> prom;
    std::future<IngestPlan> fut = prom.get_future();
    QThreadPool::globalInstance()->start(
        [&snap, root, &prom]() { prom.set_value(buildIngestPlan(root, snap)); });
    // 故意不泵事件循环（模拟 GUI 忙）——快照路径若仍依赖
    // BlockingQueuedConnection marshal 回 catalog 线程，这里会等到超时。
    const IngestPlan plan = fut.get();
    QVERIFY(clock.elapsed() < 30000);
    QVERIFY(!plan.items.isEmpty());
    for (const PlannedItem &item : plan.items)
      QCOMPARE(item.decision, QStringLiteral("skip")); // 全部重复→跳过
  }

  // ---- 扫描段取消：plan.cancelled 如实标记、items 不出场 -------------------
  void scanProgressCancelStopsPlanBuild()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = makeArea(tmp);
    QVERIFY(!root.isEmpty());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack != nullptr);

    const DataCatalog *cat = stack->importSvc->catalog();
    const LiveCatalogSource live(cat);
    const IngestPlan plan =
        buildIngestPlan(root, live, [](int, const QString &) { return false; });
    QVERIFY(plan.cancelled);
    QVERIFY(plan.issues.contains(QStringLiteral("已取消")));
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestIngestPlan tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_ingestplan.moc"
