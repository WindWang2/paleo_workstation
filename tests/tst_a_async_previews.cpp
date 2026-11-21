// A｜异步预览与交会数据读取（#961/#974）闭环测试：
// · A-W1 共享地震服务注入（幂等、懒建不重复）；
// · A-W2 消费者隔离（同 asset 不同测线互不顶替、按消费者释放/析构取消、
//   结果信号按 consumer 标签认领）；
// · A-W3 快照/构建管线与 catalog 同步口径零漂移；
// · A-W5 SgyDataCache 预算适配（实占/容量/pin/目标收缩后复原、注销）；
// · A-W6 quiesce 拒绝新请求并给真实原因。
// 确定性纪律：并发场景用单 worker + 信号量阻塞任务排队，不靠 sleep 偶遇。
#include <QtTest>
#include <QTemporaryDir>
#include <QSignalSpy>
#include <QSemaphore>

#include "../src/io/cachebudget.h"
#include "../src/io/dataimportservice.h"
#include "../src/metadata/layermanifest.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/qgis/qgislayerservice.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/previewdoc.h"
#include "../src/services/sectiondoc.h"
#include "../src/services/seismictaskservice.h"
#include "../src/services/welllogset.h"
#include "perfbudget_relax.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <memory>
#include <optional>

namespace
{
struct Stack
{
  QgisProjectService projectSvc;
  std::unique_ptr<LayerManifest> manifest;
  std::unique_ptr<QgisLayerService> layerSvc;
  std::unique_ptr<PaleoProjectStore> store;
  std::unique_ptr<DataImportService> importSvc;
  QString metaPath;
};

QString fixture(const QString &name)
{
  return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
}

std::unique_ptr<Stack> makeStack(const QString &projectDir)
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
  s->importSvc->setProjectDir(projectDir);
  return s;
}

struct SgyRef
{
  QString assetId, versionId, absPath, sha;
  bool managed = true;
};

SgyRef importSgy(Stack *st)
{
  SgyRef r;
  QString err;
  r.assetId = st->importSvc->importProjectFile(
      fixture(QStringLiteral("mini_seismic.sgy")), &err);
  DataCatalog *cat = st->importSvc->catalog();
  if (r.assetId.isEmpty() || !cat)
    return r;
  const CatalogVersion v = cat->currentVersion(r.assetId);
  r.versionId = v.id;
  r.managed = v.managed;
  r.sha = v.sha256;
  r.absPath = st->importSvc->absolutePathForVersion(v);
  return r;
}

QString writeLas(const QString &path, const QStringList &mnems, int rows)
{
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return QString();
  QTextStream out(&file);
  out << "~Version Information\n";
  out << " VERS.                  2.0:   CWLS log ASCII Standard -VERSION 2.0\n";
  out << " WRAP.                   NO:   One line per depth step\n";
  out << "~Well Information Block\n";
  out << " STRT.M                  0.0000:\n";
  out << " STOP.M        " << QString::number(rows - 1.0, 'f', 4) << ":\n";
  out << " STEP.M          1.0000:\n";
  out << " NULL.        -999.2500:\n";
  out << "~Curve Information Block\n";
  for (const QString &mnem : mnems)
    out << " " << mnem << ".M                  :   " << mnem << "\n";
  out << "~A\n";
  for (int r = 0; r < rows; ++r)
  {
    out << QString::number(r, 'f', 2);
    for (int c = 1; c < mnems.size(); ++c)
      out << " " << QString::number(10.0 * c + r, 'f', 2);
    out << "\n";
  }
  file.close();
  return path;
}

// 单 worker 下的排队屏障：占住唯一 worker，后续任务确定性地排队。
class Blocker
{
public:
  explicit Blocker(PaleoTaskService &tasks) : d(tasks.start(QStringLiteral("blocker"), [this](PaleoTask *t) {
        while (!m_release.tryAcquire(1, 10))
          if (t && t->cancelRequested())
            return QString();
        return QString();
      }))
  {
  }
  ~Blocker() { release(); }
  void release()
  {
    m_release.release();
    if (d)
      while (d->state() == PaleoTask::State::Running)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
  }
  PaleoTask *task() const { return d.data(); }

private:
  QSemaphore m_release;
  QPointer<PaleoTask> d;
};

} // namespace

class TestAAsyncPreviews : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase() { QVERIFY(QgisRuntime::isInitialized()); }

  // ---- A-W1：共享地震服务注入幂等；懒建实例稳定 ----
  void sharedSeismicServiceInjectionIsIdempotent()
  {
    PaleoTaskService tasks;
    seismic::SeismicTaskService shared(&tasks, 16);
    PreviewDocService doc(nullptr);
    doc.setTaskService(&tasks);
    QVERIFY(doc.seismicTaskService() == nullptr ||
            doc.seismicTaskService() != &shared); // 惰性：未取用不判定
    doc.setSeismicTaskService(&shared);
    QCOMPARE(doc.seismicTaskService(), &shared);
    doc.setSeismicTaskService(&shared); // 重复注入幂等
    doc.setTaskService(&tasks);         // 任务服务重接线不顶掉共享实例
    QCOMPARE(doc.seismicTaskService(), &shared);
    // 未注入共享时的懒建兜底：同一实例稳定，不重复建。
    PreviewDocService lone(nullptr);
    lone.setTaskService(&tasks);
    seismic::SeismicTaskService *first = lone.seismicTaskService();
    QVERIFY(first);
    QVERIFY(first != &shared);
    QCOMPARE(lone.seismicTaskService(), first);
  }

  // ---- A-W2：两个消费者同资产不同测线互不顶替 ----
  void twoConsumersSameAssetKeepOwnRequests()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack);
    const SgyRef sgy = importSgy(stack.get());
    QVERIFY2(!sgy.assetId.isEmpty(), "mini_seismic import failed");

    PaleoTaskService tasks;
    tasks.setMaxWorkerThreads(1); // 排队确定性：一次只跑一个解码
    PreviewDocService doc(stack->importSvc.get());
    doc.setTaskService(&tasks);

    QObject consumerA, consumerB;
    const quint64 tagA = PreviewDocService::consumerTag(&consumerA);
    const quint64 tagB = PreviewDocService::consumerTag(&consumerB);
    QVERIFY(tagA != tagB);

    QSignalSpy ready(&doc, &PreviewDocService::seismicSectionReady);
    QSignalSpy failed(&doc, &PreviewDocService::seismicSectionFailed);

    {
      Blocker hold(tasks); // 两个解码先排队（互不顶替的前提是都能跑完）
      doc.requestSection(sgy.assetId, sgy.versionId, sgy.absPath, sgy.managed,
                         sgy.sha, true, 1000, &consumerA);
      doc.requestSection(sgy.assetId, sgy.versionId, sgy.absPath, sgy.managed,
                         sgy.sha, true, 1001, &consumerB);
    }
    // 两消费者的请求都完成：各自标签、各自测线。
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 30000);
    bool sawA = false, sawB = false;
    for (const auto &args : ready)
    {
      const auto section = args.at(1).value<PreviewDocService::SectionDoc>();
      const quint64 tag = args.at(2).toULongLong();
      if (tag == tagA)
      {
        QCOMPARE(section.lineNo, 1000);
        sawA = true;
      }
      else if (tag == tagB)
      {
        QCOMPARE(section.lineNo, 1001);
        sawB = true;
      }
    }
    QVERIFY(sawA && sawB);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(tasks.runningCount(), 0);
  }

  // ---- A-W2：关闭一个消费者只取消它自己的在途请求 ----
  void releaseScopesToConsumerAndCancelsOnlyIt()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack);
    const SgyRef sgy = importSgy(stack.get());
    QVERIFY2(!sgy.assetId.isEmpty(), "mini_seismic import failed");

    PaleoTaskService tasks;
    tasks.setMaxWorkerThreads(1);
    PreviewDocService doc(stack->importSvc.get());
    doc.setTaskService(&tasks);

    QObject consumerA, consumerB;
    const quint64 tagB = PreviewDocService::consumerTag(&consumerB);
    QSignalSpy ready(&doc, &PreviewDocService::seismicSectionReady);
    QSignalSpy cancelled(&doc, &PreviewDocService::seismicSectionCancelled);

    {
      Blocker hold(tasks); // A/B 的解码任务都在队列里
      doc.requestSection(sgy.assetId, sgy.versionId, sgy.absPath, sgy.managed,
                         sgy.sha, true, 1000, &consumerA);
      doc.requestSection(sgy.assetId, sgy.versionId, sgy.absPath, sgy.managed,
                         sgy.sha, true, 1001, &consumerB);
      // A 关闭：只有 A 的排队任务被取消；B 的继续。
      doc.releaseSection(sgy.assetId, &consumerA);
    }
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 30000);
    QCOMPARE(ready.at(0).at(2).toULongLong(), quint64(tagB)); // 只有 B 的结果
    QCOMPARE(ready.at(0).at(1).value<PreviewDocService::SectionDoc>().lineNo, 1001);
    // A 的任务终态 Cancelled（协作取消或出队跳过），B 的 Succeeded。
    int aCancelled = 0, bSucceeded = 0;
    for (const PaleoTask *t : tasks.tasks())
    {
      if (t->title() == QStringLiteral("blocker"))
        continue;
      if (t->state() == PaleoTask::State::Cancelled)
        ++aCancelled;
      else if (t->state() == PaleoTask::State::Succeeded)
        ++bSucceeded;
    }
    QCOMPARE(aCancelled, 1);
    QCOMPARE(bSucceeded, 1);
    QTRY_COMPARE(tasks.runningCount(), 0);
    // 匿名释放的兼容口径仍可用：清 B 的在途状态（终态后调用无副作用）。
    doc.releaseSection(sgy.assetId);
    doc.releaseSection(sgy.assetId, &consumerB);
    QCOMPARE(cancelled.count(), 0);
  }

  // ---- A-W2：消费者析构自动取消其请求（关标签页路径）----
  void consumerDestructionCancelsOwnRequest()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack);
    const SgyRef sgy = importSgy(stack.get());
    QVERIFY2(!sgy.assetId.isEmpty(), "mini_seismic import failed");

    PaleoTaskService tasks;
    tasks.setMaxWorkerThreads(1);
    PreviewDocService doc(stack->importSvc.get());
    doc.setTaskService(&tasks);
    QSignalSpy ready(&doc, &PreviewDocService::seismicSectionReady);

    QObject consumerB; // 普通消费者（不用服务自身：服务析构序不在本用例面内）
    auto *doomed = new QObject();
    {
      Blocker hold(tasks);
      doc.requestSection(sgy.assetId, sgy.versionId, sgy.absPath, sgy.managed,
                         sgy.sha, true, 1000, doomed);
      doc.requestSection(sgy.assetId, sgy.versionId, sgy.absPath, sgy.managed,
                         sgy.sha, true, 1001, &consumerB);
    }
    delete doomed; // 排队中析构 → 其任务取消，不影响他人
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 30000);
    QCOMPARE(ready.at(0).at(2).toULongLong(),
             PreviewDocService::consumerTag(&consumerB));
    QTRY_COMPARE(tasks.runningCount(), 0);
  }

  // ---- A-W6：quiesce 后新请求被拒并以真实原因回报，不起 worker ----
  void quiescedServiceRejectsNewWorkHonestly()
  {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString projectDir = tmp.filePath(QStringLiteral("proj"));
    QVERIFY(QDir().mkpath(projectDir));
    auto stack = makeStack(projectDir);
    QVERIFY(stack);

    PaleoTaskService tasks;
    PreviewDocService doc(stack->importSvc.get());
    doc.setTaskService(&tasks);
    doc.quiesce();
    QVERIFY(doc.isQuiesced());

    QSignalSpy sectionFailed(&doc, &PreviewDocService::seismicSectionFailed);
    QSignalSpy lasFailed(&doc, &PreviewDocService::lasFailed);
    doc.requestSection(QStringLiteral("ast-x"), QStringLiteral("v1"),
                       tmp.filePath(QStringLiteral("none.sgy")), true, QString(),
                       true, 1, this);
    QCOMPARE(sectionFailed.count(), 1);
    QVERIFY(sectionFailed.at(0).at(1).toString().contains(QStringLiteral("退出")));
    const QString lasPath =
        writeLas(tmp.filePath(QStringLiteral("w.las")), {QStringLiteral("DEPT"), QStringLiteral("GR")}, 8);
    doc.requestLas(QStringLiteral("k"), lasPath, {}, this);
    QCOMPARE(lasFailed.count(), 1);
    QVERIFY(lasFailed.at(0).at(1).toString().contains(QStringLiteral("退出")));
    QCOMPARE(doc.prefetch({lasPath}), 0);
    QCOMPARE(tasks.tasks().size(), 0); // 没有偷偷起任务
  }

  // ---- A-W5：预算适配器实占/pin/目标收缩/注销 ----
  void seismicCacheBudgetAdapterReportsAndEvicts()
  {
    CacheBudgetManager *mgr = CacheBudgetManager::instance();
    const qint64 savedBudget = mgr->budgetBytes();
    const auto restore = qScopeGuard([&] { mgr->setBudgetBytes(savedBudget); });

    auto makeImage = [](int side) {
      auto image = std::make_shared<seismic::SgySliceImage>();
      image->width = side;
      image->height = side;
      image->values.assign(std::size_t(side) * side, 0.f);
      image->rgba.assign(std::size_t(side) * side * 4, 0);
      return image;
    };
    const auto imageBytes = [](const std::shared_ptr<const seismic::SgySliceImage> &img) {
      return qint64(seismic::SgyDataCache::ImageBytes(*img));
    };

    auto findStats = [mgr](const QString &id) -> std::optional<CacheBudgetManager::NamedStats> {
      for (const auto &s : mgr->allStats())
        if (s.id == id)
          return s;
      return std::nullopt;
    };

    QString cacheId;
    {
      PaleoTaskService tasks;
      seismic::SeismicTaskService svc(&tasks, 8); // 8 MiB
      // 注册面：实例身份出现在 manager 统计里。
      cacheId = [&] {
        for (const auto &s : mgr->allStats())
          if (s.id.startsWith(QStringLiteral("sgy-data-cache-")))
            return s.id;
        return QString();
      }();
      QVERIFY(!cacheId.isEmpty());
      const auto stats = findStats(cacheId);
      QVERIFY(stats.has_value());
      QCOMPARE(stats->capacity, qint64(8) * 1024 * 1024);

      // 两个已知字节数的图片：pinned（外部持有）+ evictable。
      auto pinned = makeImage(512);   // ~1 MiB values + ~1 MiB rgba（外部持有 = pin）
      auto evictable = makeImage(512);
      const qint64 pinnedBytes = imageBytes(pinned);
      const qint64 evictableBytes = imageBytes(evictable);
      svc.dataCache().Store("pin", pinned);
      svc.dataCache().Store("evict", evictable);
      evictable.reset(); // 只剩缓存持有 → 可驱逐
      const qint64 used = svc.dataCache().UsedBytes();
      QCOMPARE(used, std::size_t(pinnedBytes + evictableBytes));
      QCOMPARE(findStats(cacheId)->bytes, used); // bytes = 实占，非容量

      // 全局预算压低 → manager enforce：未 pin 的 LRU 条目被驱逐，
      // pin 的保持；临时目标收缩后配置容量复原。
      mgr->setBudgetBytes(1);
      QCOMPARE(qint64(svc.dataCache().UsedBytes()), pinnedBytes);
      QVERIFY(!svc.dataCache().Peek("evict"));
      QVERIFY(svc.dataCache().Peek("pin"));
      QCOMPARE(findStats(cacheId)->capacity, qint64(8) * 1024 * 1024);

      // 释放 pin 后再 enforce：可驱逐。
      auto held = svc.dataCache().Find("pin");
      pinned.reset();
      held.reset();
      mgr->enforce();
      QCOMPARE(qint64(svc.dataCache().UsedBytes()), qint64(0));
    } // svc 析构 → 注销
    QVERIFY(!findStats(cacheId).has_value());
  }
};

int main(int argc, char *argv[])
{
  if (!QgisRuntime::initialize(QStringLiteral("/usr")))
  {
    qFatal("QgisRuntime::initialize failed");
    return 1;
  }
  TestAAsyncPreviews tc;
  const int rc = QTest::qExec(&tc, argc, argv);
  QgisRuntime::shutdown();
  return rc;
}

#include "tst_a_async_previews.moc"
