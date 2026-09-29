// 层：测试壳
// P5 Phase 6 性能与可靠性测试（D6.1–D6.8）：切片时延预算（命中 <50ms/
// 未命中 <500ms 入基线）、3D LOD 帧率 ≥15fps、错误分类分级、内存预算评估
// （D6.3 同形接口 + D6.8 超限警告）、并发闸 ≤4、取消无悬挂、会话/转码
// 自动保存点。
#include <QtTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QProcess>
#include <QTemporaryDir>

#include <atomic>
#include <cmath>
#include <filesystem>

#include "Engine/Sdk.h"

#include "../src/domain/seismic/sgyvolume.h"
#include "../src/services/paleotaskservice.h"
#include "../src/services/seismictaskservice.h"
#include "../src/ui/seismic3d/seismiccameracontroller.h"
#include "../src/ui/seismic3d/seismicslicerenderer.h"
#include "../src/ui/seismic3d/volumeframerenderer.h"

using namespace seismic;

namespace {
constexpr double kSliceHitBudgetMs = 50.0;
constexpr double kSliceMissBudgetMs = 500.0;
constexpr double kFpsBudget = 15.0;
} // namespace

class TestSeismicBudgets : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    qputenv("PALEO_UI_CAPTURE", "0");
    // 220MB 生产形状夹具（与 perf/baseline 共享缓存目录）
    const QString perfDir = QStringLiteral(PALEO_SEISMIC_PERF_DIR);
    bigSgy_ = perfDir + QStringLiteral("/perf_big.sgy");
    const qint64 minBytes = 200ll * 1024 * 1024;
    if (QFileInfo(bigSgy_).size() < minBytes)
    {
      QDir().mkpath(perfDir);
      QCOMPARE(QProcess::execute(
                   QStringLiteral(PALEO_PYTHON3),
                   {QStringLiteral(PALEO_SEGY_FIXTURE_TOOL),
                    QStringLiteral("--out"), bigSgy_,
                    QStringLiteral("--mb"), QStringLiteral("220")}), 0);
    }
    QVERIFY(QFileInfo(bigSgy_).size() >= minBytes);
    Q_INIT_RESOURCE(seismic_shaders); // VolumeFrameRenderer 的 axis shader
  }

  // ---- D6.1 切片时延预算：未命中 <500ms / 引擎缓存命中 <50ms ----
  void sliceLatencyBudgets()
  {
    engine::Status st;
    auto ds = sdk::Dataset::Open(std::filesystem::path(bigSgy_.toStdString()),
                                 sdk::OpenOptions{}, st);
    QVERIFY2(ds, st.message.c_str());
    ds->ClearCaches(); // 强制冷

    engine::Slice2D slice;
    const int il = ds->Metadata().inlineAxis.origin + 17;
    QElapsedTimer clock;
    clock.start();
    QVERIFY(ds->ReadInline(il, slice).ok());
    const double missMs = clock.elapsed();
    qInfo("slice miss: %.1f ms (budget %.0f)", missMs, kSliceMissBudgetMs);
    QVERIFY2(missMs < kSliceMissBudgetMs,
             qPrintable(QStringLiteral("cold slice %1 ms over %2").arg(missMs).arg(kSliceMissBudgetMs)));

    clock.restart();
    QVERIFY(ds->ReadInline(il, slice).ok());
    const double hitMs = clock.elapsed();
    qInfo("slice hit: %.1f ms (budget %.0f)", hitMs, kSliceHitBudgetMs);
    QVERIFY2(hitMs < kSliceHitBudgetMs,
             qPrintable(QStringLiteral("hot slice %1 ms over %2").arg(hitMs).arg(kSliceHitBudgetMs)));

  }

  // ---- D6.2 3D 帧率预算：LOD 交互 ≥15fps ----
  void fpsBudgetLod()
  {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QOffscreenSurface surface;
    surface.setFormat(format);
    surface.create();
    if (!surface.isValid())
      QSKIP("offscreen surface unavailable");
    QOpenGLContext context;
    context.setFormat(format);
    if (!context.create() || !context.makeCurrent(&surface))
      QSKIP("OpenGL context could not be created in this environment");
    QOpenGLFunctions_3_3_Core gl;
    if (!gl.initializeOpenGLFunctions())
      QSKIP("OpenGL 3.3 Core functions not available");

    SgyVolume volume;
    std::string err;
    QVERIFY2(volume.Load(bigSgy_.toStdString(), err), err.c_str());

    SeismicSliceRenderer renderer;
    QVERIFY(renderer.Initialize(&gl));
    SgySliceImage img;
    const int midIl = volume.InlineValues()[volume.InlineValues().size() / 2];
    QVERIFY(volume.ExtractSlice(SgySliceType::Inline, midIl, img, err));
    QVERIFY(renderer.UpdateSlice(&gl, SeismicSliceSlot::Inline, volume,
                                 SgySliceType::Inline, midIl, img));
    renderer.SetSliceAlpha(0.7f); // 交互典型态（混合开）

    VolumeFrameRenderer frame;
    QVERIFY(frame.Initialize(&gl));
    frame.UpdateFromVolume(&gl, volume);

    GLuint fbo = 0, colorTex = 0, depthRbo = 0;
    gl.glGenFramebuffers(1, &fbo);
    gl.glGenTextures(1, &colorTex);
    gl.glBindTexture(GL_TEXTURE_2D, colorTex);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 800, 600, 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, nullptr);
    gl.glGenRenderbuffers(1, &depthRbo);
    gl.glBindRenderbuffer(GL_RENDERBUFFER, depthRbo);
    gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 800, 600);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRbo);
    if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      QSKIP("baseline FBO incomplete");
    gl.glViewport(0, 0, 800, 600);
    gl.glEnable(GL_DEPTH_TEST);

    SeismicCameraController camera;
    camera.FitToBounds(glm::vec3(-3, -2.2, -3), glm::vec3(3, 2.2, 3), 4.0 / 3.0);
    const glm::mat4 proj = camera.BuildProjectionMatrix(4.0 / 3.0);

    for (int i = 0; i < 5; ++i) { // 预热
      gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      renderer.Render(&gl, camera.BuildViewMatrix(), proj);
      frame.Render(&gl, camera.BuildViewMatrix(), proj);
    }
    gl.glFinish();

    constexpr int kFrames = 120;
    QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < kFrames; ++i) {
      camera.Rotate(-0.3f, 0.1f); // 交互中持续旋转（LOD 态）
      gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      renderer.Render(&gl, camera.BuildViewMatrix(), proj);
      frame.Render(&gl, camera.BuildViewMatrix(), proj);
      gl.glFinish();
    }
    const double fps = kFrames * 1000.0 / clock.elapsed();
    qInfo("3D fps (LOD, synced): %.1f (budget %.0f)", fps, kFpsBudget);
    QVERIFY2(fps >= kFpsBudget,
             qPrintable(QStringLiteral("%1 fps under budget %2").arg(fps).arg(kFpsBudget)));

    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glDeleteFramebuffers(1, &fbo);
    gl.glDeleteTextures(1, &colorTex);
    gl.glDeleteRenderbuffers(1, &depthRbo);
  }

  // ---- D6.6 错误分类分级 ----
  void errorClassification()
  {
    using Kind = SeismicTaskService::SeismicErrorCategory::Kind;
    auto kindOf = [](const QString &err) {
      return SeismicTaskService::SeismicErrorCategory::classify(err).kind;
    };
    QCOMPARE(kindOf(QStringLiteral("文件不存在: /tmp/x.sgy")), Kind::FileMissing);
    QCOMPARE(kindOf(QStringLiteral("无法打开 SEG-Y 文件")), Kind::FileMissing);
    QCOMPARE(kindOf(QStringLiteral("索引损坏或不完整")), Kind::IndexCorrupt);
    QCOMPARE(kindOf(QStringLiteral("workspace metadata corrupt")), Kind::IndexCorrupt);
    QCOMPARE(kindOf(QStringLiteral("内存超限 bad_alloc")), Kind::MemoryBudget);
    QCOMPARE(kindOf(QStringLiteral("任务已取消")), Kind::Cancelled);
    QCOMPARE(kindOf(QStringLiteral("别的什么错")), Kind::Other);

    // GL 分支
    const auto gl = SeismicTaskService::SeismicErrorCategory::classify(QString(), true);
    QCOMPARE(gl.kind, Kind::GlUnavailable);
    QVERIFY(gl.userText.contains(QStringLiteral("OpenGL")));

    // userText 总有可读文案
    const auto e = SeismicTaskService::SeismicErrorCategory::classify(
        QStringLiteral("索引损坏或不完整"));
    QVERIFY(!e.userText.isEmpty());
  }

  // ---- D6.3/D6.8 内存预算评估 ----
  void memoryBudgetAssessment()
  {
    // 8GB RAM（预算 4GB）、5GB 体 → 超限 + 建议文案
    const auto over = SeismicTaskService::assessMemoryBudget(5ll << 30, 8ll << 30);
    QVERIFY(over.overBudget);
    QCOMPARE(over.budgetBytes, 4ll << 30);
    QVERIFY(over.recommendation.contains(QStringLiteral("分页")));

    // 2GB 体 ≤ 4GB 预算 → 不超限
    const auto ok = SeismicTaskService::assessMemoryBudget(2ll << 30, 8ll << 30);
    QVERIFY(!ok.overBudget);
    QVERIFY(ok.recommendation.isEmpty());

    // 真机 RAM 查询 > 0（D6.3 同形接口）
    QVERIFY(SeismicTaskService::totalRamBytes() > 1024ll * 1024 * 1024);

    // 服务内存估计 > 0（数据缓存预算至少计入）
    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks, 16);
    QVERIFY(svc.estimatedMemoryBytes() >= 16ll * 1024 * 1024);
  }

  // ---- D6.4 并发闸：≤4 个地震任务同时执行 ----
  void concurrencyGateMax4()
  {
    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks);

    std::atomic<int> concurrent{0};
    std::atomic<int> maxConcurrent{0};
    std::atomic<int> completed{0};
    constexpr int kJobs = 8;

    for (int i = 0; i < kJobs; ++i) {
      svc.startBounded(QStringLiteral("闸测试 %1").arg(i),
                       [&](PaleoTask *) -> QString {
                         const int now = concurrent.fetch_add(1) + 1;
                         int prev = maxConcurrent.load();
                         while (now > prev && !maxConcurrent.compare_exchange_weak(prev, now)) {}
                         QThread::msleep(120);
                         concurrent.fetch_sub(1);
                         completed.fetch_add(1);
                         return QString();
                       });
    }
    // 等全部完成（闸保证 ≤4 并发——总时长 ≈ 2 批 × 120ms）
    QElapsedTimer clock;
    clock.start();
    while (completed.load() < kJobs && clock.elapsed() < 30000)
      QApplication::processEvents(QEventLoop::AllEvents, 20);
    QCOMPARE(completed.load(), kJobs);
    QVERIFY2(maxConcurrent.load() <= SeismicTaskService::kMaxConcurrentTasks,
             qPrintable(QStringLiteral("observed %1 concurrent > %2")
                        .arg(maxConcurrent.load())
                        .arg(SeismicTaskService::kMaxConcurrentTasks)));
    qInfo("gate: %d jobs, max concurrent %d", kJobs, maxConcurrent.load());
    QCOMPARE(SeismicTaskService::kMaxConcurrentTasks, 4);
  }

  // ---- D6.5 取消无悬挂：排队中取消的任务直接跳过执行 ----
  void cancelQueuedNoHang()
  {
    PaleoTaskService tasks;
    SeismicTaskService svc(&tasks);

    std::atomic<int> executed{0};
    // 占满 4 槽的长任务
    for (int i = 0; i < 4; ++i)
      svc.startBounded(QStringLiteral("占槽 %1").arg(i), [&](PaleoTask *t) -> QString {
        QElapsedTimer w;
        w.start();
        while (w.elapsed() < 250 && !t->cancelRequested())
          QThread::msleep(10);
        executed.fetch_add(1);
        return QString();
      });
    // 第 5 个：排队 + 立刻取消 → 获槽后必须直接跳过（不执行、不悬挂）
    PaleoTask *queued = svc.startBounded(QStringLiteral("排队即取消"), [&](PaleoTask *) -> QString {
      executed.fetch_add(1); // 不应执行到
      return QString();
    });
    queued->requestCancel();

    QElapsedTimer clock;
    clock.start();
    while (svc.activeTaskCount() > 0 && clock.elapsed() < 10000)
      QApplication::processEvents(QEventLoop::AllEvents, 20);
    QCOMPARE(svc.activeTaskCount(), 0); // 全部收尾——无悬挂
    QVERIFY(clock.elapsed() < 5000);    // 秒级收尾（不是死等）
  }

  // ---- D6.7 自动保存点：解释会话即时落盘（转码续跑位图由引擎测过）----
  void sessionAutosavePoint()
  {
    QTemporaryDir dir;
    SeismicInterpretationSession session;
    session.sourceSgyPath = dir.filePath("auto.sgy");
    session.picks.append({1, 1000, 2000, 64.0, 32, 1.0f, QStringLiteral("A"), QStringLiteral("H1")});
    QString err;
    QVERIFY(SeismicTaskService::saveSession(session, &err));
    // 伴生文件即保存点——进程崩溃后 loadSession 可恢复
    SeismicInterpretationSession restored;
    QVERIFY(SeismicTaskService::loadSession(session.sourceSgyPath, restored, &err));
    QCOMPARE(restored.picks.size(), 1);
    QCOMPARE(restored.picks.first().twtMs, 64.0);
  }

private:
  QString bigSgy_;
};

QTEST_MAIN(TestSeismicBudgets)
#include "tst_seismic_budgets.moc"
