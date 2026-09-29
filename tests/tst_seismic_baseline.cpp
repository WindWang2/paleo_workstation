// 层：测试壳
// BASELINE 实测夹具（P5 Phase 0 → docs/seismic/BASELINE.md）：
// 复用 tst_seismic_perf 的 ≥200MB 生产形状体，对索引冷/热、切片冷/热、
// 双通道转码、后端打开、体素窗口、离屏 3D 帧率、峰值内存做一轮测量并
// 以 "BASELINE <metric> = <value>" 行打印。断言只做量级合理性兜底
// （共享机宽裕），精确预算闸门在 tst_seismic_perf / tst_seismic_budgets。
#include <QtTest>
#ifdef _WIN32 // 编译器原生宏（Q_OS_WIN 要等 QtTest 引入 qglobal 后才有）
#include <windows.h>
#include <psapi.h>
#endif
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QProcess>
#include <QTemporaryDir>

#include <cmath>
#include <filesystem>
#include <fstream>

#include "Data/Sgy/SgyIndex.h"
#include "Data/Sgy/SgyIndexBuilder.h"
#include "Data/Sgy/SgyIndexCache.h"
#include "Data/Sgy/SgyVolume.h"
#include "Engine/PagedPipeline.h"
#include "Engine/Sdk.h"
#include "Engine/TranscodeJob.h"

#include <glm/gtc/matrix_transform.hpp>
#include "Engine/Types.h"

#include "../src/ui/seismic3d/seismiccameracontroller.h"
#include "../src/ui/seismic3d/seismicslicerenderer.h"
#include "../src/ui/seismic3d/volumeframerenderer.h"

using namespace seismic;

namespace {

void report(const char *metric, double value, const char *unit)
{
    qInfo("BASELINE %s = %.1f %s", metric, value, unit);
}

// 进程峰值 RSS（KB）：Linux 读 /proc/self/status VmHWM；Windows 用
// GetProcessMemoryInfo 的 PeakWorkingSetSize。
qint64 peakRssKb()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<qint64>(pmc.PeakWorkingSetSize) / 1024;
    return -1;
#else
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmHWM:") {
            qint64 kb = 0;
            status >> kb;
            return kb;
        }
        status.ignore(4096, '\n');
    }
    return -1;
#endif
}

} // namespace

class TestSeismicBaseline : public QObject
{
    Q_OBJECT

private:
    QString bigSgy_;
    std::filesystem::path sgyPath_;
    QTemporaryDir transcodeDir_;

private slots:
    void initTestCase()
    {
        const QString perfDir = QStringLiteral(PALEO_SEISMIC_PERF_DIR);
        QVERIFY(QDir().mkpath(perfDir));
        bigSgy_ = perfDir + QStringLiteral("/perf_big.sgy");

        const qint64 minBytes = 200ll * 1024 * 1024;
        if (QFileInfo(bigSgy_).size() < minBytes) {
            const int rc = QProcess::execute(
                QStringLiteral(PALEO_PYTHON3),
                {QStringLiteral(PALEO_SEGY_FIXTURE_TOOL),
                 QStringLiteral("--out"), bigSgy_,
                 QStringLiteral("--mb"), QStringLiteral("220")});
            QVERIFY2(rc == 0, "make_segy_fixture.py --mb 220 failed");
        }
        QVERIFY2(QFileInfo(bigSgy_).size() >= minBytes,
                 "perf fixture must be >= 200 MB");
        sgyPath_ = std::filesystem::path(bigSgy_.toStdString());
        qInfo("baseline fixture: %s (%.1f MiB)", qPrintable(bigSgy_),
              QFileInfo(bigSgy_).size() / 1048576.0);
        Q_INIT_RESOURCE(seismic_shaders);
    }

    // 冷索引：显式 Remove（伴生 .sgyidx + 集中缓存）后全卷道头扫描
    void indexColdFullScan()
    {
        std::string err;
        QVERIFY(SgyIndexCache::Remove(sgyPath_, err));

        SgyIndexPtr index;
        QElapsedTimer clock;
        clock.start();
        QVERIFY2(SgyIndexBuilder::Build(sgyPath_, index, err, {}), err.c_str());
        const double ms = clock.elapsed();
        report("index_cold_full_scan", ms, "ms");
        report("index_traces", double(index->traces.size()), "traces");
        QVERIFY2(ms < 120000.0, "cold index over 120 s sanity bound");

        // 顺手落盘，为后续转码/打开提供热索引路径
        QVERIFY(SgyIndexCache::Save(index, err));
    }

    // 热索引：伴生/集中缓存加载
    void indexWarmCache()
    {
        std::string reason;
        QElapsedTimer clock;
        clock.start();
        SgyIndexPtr index = SgyIndexCache::Load(sgyPath_, reason);
        const double ms = clock.elapsed();
        QVERIFY2(index, reason.c_str());
        report("index_warm_cache", ms, "ms");
        QVERIFY2(ms < 5000.0, "warm index load over 5 s sanity bound");
    }

    // 直读后端：Open + 首条 inline 剖面（冷）/ 立即重读（引擎 SliceCache 热）
    void directSliceColdAndHot()
    {
        engine::Status st;
        QElapsedTimer clock;
        clock.start();
        auto ds = sdk::Dataset::Open(sgyPath_, sdk::OpenOptions{}, st);
        const double openMs = clock.elapsed();
        QVERIFY2(ds, st.message.c_str());
        report("open_direct", openMs, "ms");

        engine::Slice2D slice;
        clock.restart();
        QVERIFY(ds->ReadInline(ds->Metadata().inlineAxis.origin + 5, slice, nullptr).ok());
        report("slice_inline_cold_direct", clock.elapsed(), "ms");

        clock.restart();
        QVERIFY(ds->ReadInline(ds->Metadata().inlineAxis.origin + 5, slice, nullptr).ok());
        report("slice_inline_hot_direct", clock.elapsed(), "ms");

        const int midSample = int(ds->Metadata().sampleCount) / 2;
        clock.restart();
        QVERIFY(ds->ReadTimeSlice(midSample, slice, nullptr).ok());
        report("slice_time_cold_direct", clock.elapsed(), "ms");
        clock.restart();
        QVERIFY(ds->ReadTimeSlice(midSample, slice, nullptr).ok());
        report("slice_time_hot_direct", clock.elapsed(), "ms");
        QVERIFY2(clock.elapsed() < 5000.0, "hot time slice over 5 s sanity bound");
    }

    // .sf3c 转码（临时目录，不影响夹具旁产物）+ 质量字段采样
    void transcodeSf3c()
    {
        const auto base = std::filesystem::path(transcodeDir_.path().toStdString())
            / "baseline.sf3c";
        engine::CancelToken cancel;
        QElapsedTimer clock;
        clock.start();
        const auto result = engine::TranscodeSegyToWorkspace(
            sgyPath_, base, engine::TranscodeOptions{}, &cancel,
            [](const engine::TranscodeProgress &) { return true; });
        const double ms = clock.elapsed();
        QVERIFY2(result.status.ok(), result.status.message.c_str());
        report("transcode_sf3c_total", ms, "ms");
        report("transcode_sf3c_traces", double(result.tracesRead), "traces");
        report("transcode_sf3c_out_mb",
               double(result.bytesWritten) / 1048576.0, "MiB");
        QVERIFY2(ms < 300000.0, "sf3c transcode over 300 s sanity bound");

        engine::Status st;
        clock.restart();
        auto ds = sdk::Dataset::Open(base, sdk::OpenOptions{}, st);
        report("open_workspace", clock.elapsed(), "ms");
        QVERIFY2(ds, st.message.c_str());

        engine::Slice2D slice;
        clock.restart();
        QVERIFY(ds->ReadInline(ds->Metadata().inlineAxis.origin + 5, slice, nullptr).ok());
        report("slice_inline_cold_workspace", clock.elapsed(), "ms");
        clock.restart();
        QVERIFY(ds->ReadInline(ds->Metadata().inlineAxis.origin + 5, slice, nullptr).ok());
        report("slice_inline_hot_workspace", clock.elapsed(), "ms");
        QVERIFY2(clock.elapsed() < 2000.0, "workspace hot slice over 2 s sanity bound");
    }

    // .sf3p 金字塔（L0+L1+L2 全建）+ paged 打开 + 体素窗口
    void transcodeSf3pAndVoxel()
    {
        const auto l0 = std::filesystem::path(transcodeDir_.path().toStdString())
            / "baseline.sf3p";
        engine::CancelToken cancel;
        engine::PagedPipelineOptions opts;
        QElapsedTimer clock;
        clock.start();
        const auto result = engine::BuildPagedPyramid(
            sgyPath_, l0, opts, &cancel,
            [](const engine::PagedPipelineProgress &) { return true; });
        const double ms = clock.elapsed();
        QVERIFY2(result.status.ok(), result.status.message.c_str());
        report("transcode_sf3p_pyramid", ms, "ms");
        QVERIFY2(ms < 600000.0, "sf3p pyramid over 600 s sanity bound");
        report("transcode_sf3p_l0_mb",
               std::filesystem::file_size(l0) / 1048576.0, "MiB");
        report("transcode_sf3p_l1_mb",
               std::filesystem::file_size(engine::PagedLodPath(l0, 1)) / 1048576.0,
               "MiB");
        report("transcode_sf3p_l2_mb",
               std::filesystem::file_size(engine::PagedLodPath(l0, 2)) / 1048576.0,
               "MiB");

        engine::Status st;
        sdk::OpenOptions openOpts;
        openOpts.backend = sdk::Backend::Paged;
        openOpts.progressiveLod = true;
        clock.restart();
        auto ds = sdk::Dataset::Open(l0, openOpts, st);
        report("open_paged", clock.elapsed(), "ms");
        QVERIFY2(ds, st.message.c_str());

        const auto &meta = ds->Metadata();
        engine::VoxelWindowRequest req;
        req.inlineBegin = meta.inlineAxis.origin + meta.inlineAxis.count / 4;
        req.inlineCount = 64;
        req.xlineBegin = meta.xlineAxis.origin + meta.xlineAxis.count / 4;
        req.xlineCount = 64;
        req.sampleBegin = meta.sampleCount / 4;
        req.sampleCount = 64;
        engine::VoxelWindow win;
        clock.restart();
        QVERIFY(ds->ReadVoxelWindow(req, win).ok());
        report("voxel_window_64cubed_paged", clock.elapsed(), "ms");
        QVERIFY2(clock.elapsed() < 5000.0, "voxel window over 5 s sanity bound");
    }

    // 离屏 3D 帧率（输出 GL_RENDERER；真机 vsync 数值以手动验收为准）
    void fps3dOffscreen()
    {
        QSurfaceFormat format;
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CoreProfile);
        format.setRenderableType(QSurfaceFormat::OpenGL);

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
        QVERIFY2(volume.Load(sgyPath_, err), err.c_str());

        SeismicSliceRenderer renderer;
        QVERIFY(renderer.Initialize(&gl));

        SgySliceImage img;
        const int midInline = volume.InlineValues()[volume.InlineValues().size() / 2];
        const int midXline = volume.XlineValues()[volume.XlineValues().size() / 2];
        const int midSample = volume.SampleMax() / 2;
        QVERIFY(volume.ExtractSlice(SgySliceType::Inline, midInline, img, err));
        QVERIFY(renderer.UpdateSlice(&gl, SeismicSliceSlot::Inline, volume,
                                     SgySliceType::Inline, midInline, img));
        QVERIFY(volume.ExtractSlice(SgySliceType::Xline, midXline, img, err));
        QVERIFY(renderer.UpdateSlice(&gl, SeismicSliceSlot::Crossline, volume,
                                     SgySliceType::Xline, midXline, img));
        QVERIFY(volume.ExtractSlice(SgySliceType::Time, midSample, img, err));
        QVERIFY(renderer.UpdateSlice(&gl, SeismicSliceSlot::Time, volume,
                                     SgySliceType::Time, midSample, img));

        VolumeFrameRenderer frame;
        QVERIFY(frame.Initialize(&gl));
        frame.UpdateFromVolume(&gl, volume);

        // 离屏默认帧缓冲无真实栅格化面——绑 800x600 FBO 才能测出软渲染帧成本
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
        gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D, colorTex, 0);
        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                     GL_RENDERBUFFER, depthRbo);
        QVERIFY2(gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                 "baseline FBO incomplete");
        gl.glViewport(0, 0, 800, 600);
        gl.glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
        gl.glEnable(GL_DEPTH_TEST);

        SeismicCameraController camera;
        camera.FitToBounds(glm::vec3(-3.f, -3.f, -2.2f), glm::vec3(3.f, 3.f, 2.2f));

        const glm::mat4 proj = glm::perspective(
            glm::radians(45.0f), 800.0f / 600.0f, 0.1f, 1000.0f);

        // 预热（首帧含着色器/纹理懒初始化），随后 240 帧均速
        for (int i = 0; i < 10; ++i) {
            gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            renderer.Render(&gl, camera.BuildViewMatrix(), proj);
            frame.Render(&gl, camera.BuildViewMatrix(), proj);
        }
        gl.glFinish();

        qInfo("BASELINE gl_renderer = %s",
              reinterpret_cast<const char *>(gl.glGetString(GL_RENDERER)));
        constexpr int kFrames = 240;
        QElapsedTimer clock;
        clock.start();
        for (int i = 0; i < kFrames; ++i) {
            camera.Rotate(-0.2f, 0.0f); // 模拟交互中持续旋转
            gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            renderer.Render(&gl, camera.BuildViewMatrix(), proj);
            frame.Render(&gl, camera.BuildViewMatrix(), proj);
        }
        gl.glFinish();
        const double ms = clock.elapsed();
        report("fps_3d_offscreen_pipelined", kFrames * 1000.0 / ms, "fps");
        report("frame_3d_offscreen_ms", ms / kFrames, "ms");

        // 逐帧 glFinish：交互最坏情形（每帧同步等栅格化完）
        clock.restart();
        for (int i = 0; i < kFrames; ++i) {
            camera.Rotate(-0.2f, 0.0f);
            gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            renderer.Render(&gl, camera.BuildViewMatrix(), proj);
            frame.Render(&gl, camera.BuildViewMatrix(), proj);
            gl.glFinish();
        }
        const double syncMs = clock.elapsed();
        report("fps_3d_offscreen_synced", kFrames * 1000.0 / syncMs, "fps");
        report("frame_3d_offscreen_synced_ms", syncMs / kFrames, "ms");

        gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.glDeleteFramebuffers(1, &fbo);
        gl.glDeleteTextures(1, &colorTex);
        gl.glDeleteRenderbuffers(1, &depthRbo);
    }

    void peakMemory()
    {
        const qint64 kb = peakRssKb();
        QVERIFY(kb > 0);
        report("peak_rss", kb / 1024.0, "MiB");
    }
};

QTEST_MAIN(TestSeismicBaseline)
#include "tst_seismic_baseline.moc"
