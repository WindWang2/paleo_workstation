// 层：测试壳
// wave/deepen-perf A4：966MiB（1013MB）真工区只读复测——docs/seismic/BASELINE.md
// §8 的自动化数据源。环境变量（其一）：
//   PALEO_SEISMIC_REAL_SGY=/…/200P_seismic.sgy
//   PALEO_REAL_PROJECT_AREA=/…/project_area（自动拼 地震体/200P_seismic.sgy）
// 未设置时整套跳过（CI 无本地数据）。全程只读：引擎索引缓存只落集中式缓存
// 目录（ctest 沙箱 HOME），源目录零写入（收尾断言文件清单逐字节不变）。
// 输出行格式 `BASELINE <metric> = <value>`，手工誊入 docs/seismic/BASELINE.md。
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QList>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>

#include <cmath>
#include <filesystem>

#include "Engine/Sdk.h"

#include "../src/domain/seismic/sgyvolume.h"
#include "../src/ui/seismic3d/seismiccameracontroller.h"
#include "../src/ui/seismic3d/seismicslicerenderer.h"
#include "../src/ui/seismic3d/volumeframerenderer.h"

using namespace seismic;

namespace {
QString resolveRealSgy()
{
  const QString direct = qEnvironmentVariable("PALEO_SEISMIC_REAL_SGY");
  if (!direct.isEmpty())
    return direct;
  const QString area = qEnvironmentVariable("PALEO_REAL_PROJECT_AREA");
  if (area.isEmpty())
    return QString();
  return area + QStringLiteral("/地震体/200P_seismic.sgy");
}

QList<QPair<QString, qint64>> snapshotDir(const QString &dirPath)
{
  QList<QPair<QString, qint64>> out;
  const QDir dir(dirPath);
  const auto entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
  for (const QFileInfo &fi : entries)
    out.append({fi.fileName(), fi.size()});
  return out;
}
} // namespace

class TestSeismicRealArea : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    realSgy_ = resolveRealSgy();
    if (realSgy_.isEmpty() || !QFile::exists(realSgy_))
      QSKIP("PALEO_SEISMIC_REAL_SGY / PALEO_REAL_PROJECT_AREA not set — real-area metrics skipped");
    sourceDirBefore_ = snapshotDir(QFileInfo(realSgy_).absolutePath());
    Q_INIT_RESOURCE(seismic_shaders);
  }

  void cleanupTestCase()
  {
    if (realSgy_.isEmpty())
      return;
    // 只读契约：源目录文件清单（名+字节）逐项不变
    const auto after = snapshotDir(QFileInfo(realSgy_).absolutePath());
    QCOMPARE(after.size(), sourceDirBefore_.size());
    for (int i = 0; i < sourceDirBefore_.size(); ++i)
      QCOMPARE(after[i], sourceDirBefore_[i]);
  }

  // 冷开（含索引顺序扫描）→ 几何 → 切片冷/热 → 暖开（缓存命中）→ 体窗 → 任意线
  void coldIndexSlicesAndVoxel()
  {
    namespace sdk = seismic::sdk;
    engine::Status st;
    QElapsedTimer clock;

    clock.start();
    auto ds = sdk::Dataset::Open(std::filesystem::path(realSgy_.toStdString()),
                                 sdk::OpenOptions{}, st);
    const double openColdMs = double(clock.elapsed());
    QVERIFY2(ds != nullptr && st.ok(), st.message.c_str());
    qInfo("BASELINE real_open_cold_ms = %.0f", openColdMs);

    const engine::DatasetMetadata &meta = ds->Metadata();
    qInfo("BASELINE real_geometry = traces=%lld samples=%d inl=[%d..%d] xl=[%d..%d]",
          static_cast<long long>(meta.traceCount), meta.sampleCount,
          meta.inlineMin, meta.inlineMax, meta.xlineMin, meta.xlineMax);
    QCOMPARE(meta.traceCount, qlonglong{263451});

    engine::Slice2D slice;
    const int midIl = meta.inlineAxis.ValueAt(meta.inlineAxis.count / 2);
    const int midXl = meta.xlineAxis.ValueAt(meta.xlineAxis.count / 2);
    const int midSample = meta.sampleCount / 2;

    clock.restart();
    QVERIFY2(ds->ReadInline(midIl, slice).ok(), "cold inline");
    const double ilColdMs = double(clock.elapsed());
    qInfo("BASELINE real_slice_inline_cold_ms = %.0f (%d x %d)", ilColdMs, slice.width, slice.height);

    clock.restart();
    QVERIFY(ds->ReadInline(midIl, slice).ok());
    qInfo("BASELINE real_slice_inline_hot_ms = %.1f", double(clock.elapsed()));

    clock.restart();
    QVERIFY2(ds->ReadTimeSlice(midSample, slice).ok(), "cold time slice");
    qInfo("BASELINE real_slice_time_cold_ms = %.0f (%d x %d)",
          double(clock.elapsed()), slice.width, slice.height);
    clock.restart();
    QVERIFY(ds->ReadTimeSlice(midSample, slice).ok());
    qInfo("BASELINE real_slice_time_hot_ms = %.1f", double(clock.elapsed()));

    // 暖开：同进程重开（缓存命中路径——冷扫描已发布集中式缓存）
    clock.restart();
    auto ds2 = sdk::Dataset::Open(std::filesystem::path(realSgy_.toStdString()),
                                  sdk::OpenOptions{}, st);
    const double openWarmMs = double(clock.elapsed());
    QVERIFY(ds2 != nullptr && st.ok());
    qInfo("BASELINE real_open_warm_ms = %.1f", openWarmMs);
    clock.restart();
    QVERIFY2(ds2->ReadCrossline(midXl, slice).ok(), "warm-open crossline");
    qInfo("BASELINE real_slice_xline_warmopen_ms = %.0f (%d x %d)",
          double(clock.elapsed()), slice.width, slice.height);

    // 体窗 64³（直读后端逐道顺序——3D 堆叠合并通道在直读后端的口径参考）
    engine::VoxelWindowRequest req;
    req.inlineBegin = midIl;
    req.xlineBegin = midXl;
    req.sampleBegin = midSample - 32;
    req.inlineCount = 64;
    req.xlineCount = 64;
    req.sampleCount = 64;
    engine::VoxelWindow window;
    clock.restart();
    const auto vst = ds->ReadVoxelWindow(req, window);
    qInfo("BASELINE real_voxel_64cubed_ms = %.0f (%s)", double(clock.elapsed()),
          vst.ok() ? "ok" : vst.message.c_str());

    // 任意线剖面（useReadPlan 去重+范围合并）
    engine::SectionRequest section;
    section.pathPoints = {{meta.inlineMin, meta.xlineMin},
                          {meta.inlineMax, meta.xlineMax}};
    section.useReadPlan = true;
    clock.restart();
    QVERIFY2(ds->ReadSection(section, slice).ok(), "arbitrary section");
    qInfo("BASELINE real_section_diag_ms = %.0f (%d x %d)",
          double(clock.elapsed()), slice.width, slice.height);
  }

  // 离屏 3D 帧率（真工区 IL 切片纹理 + 线框；GL 不可用则跳过）
  void fps3dRealSlice()
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
    QVERIFY2(volume.Load(realSgy_.toStdString(), err), err.c_str());
    SgySliceImage img;
    const int midIl = volume.InlineValues()[volume.InlineValues().size() / 2];
    QVERIFY2(volume.ExtractSlice(SgySliceType::Inline, midIl, img, err), err.c_str());

    SeismicSliceRenderer renderer;
    QVERIFY(renderer.Initialize(&gl));
    QVERIFY(renderer.UpdateSlice(&gl, SeismicSliceSlot::Inline, volume,
                                 SgySliceType::Inline, midIl, img));
    renderer.SetSliceAlpha(0.7f);
    VolumeFrameRenderer frame;
    QVERIFY(frame.Initialize(&gl));
    frame.UpdateFromVolume(&gl, volume);

    GLuint fbo = 0, colorTex = 0, depthRbo = 0;
    gl.glGenFramebuffers(1, &fbo);
    gl.glGenTextures(1, &colorTex);
    gl.glBindTexture(GL_TEXTURE_2D, colorTex);
    gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 800, 600, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
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
    for (int i = 0; i < 5; ++i) {
      gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      renderer.Render(&gl, camera.BuildViewMatrix(), proj);
      frame.Render(&gl, camera.BuildViewMatrix(), proj);
    }
    gl.glFinish();
    constexpr int kFrames = 120;
    QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < kFrames; ++i) {
      camera.Rotate(-0.3f, 0.1f);
      gl.glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      renderer.Render(&gl, camera.BuildViewMatrix(), proj);
      frame.Render(&gl, camera.BuildViewMatrix(), proj);
      gl.glFinish();
    }
    qInfo("BASELINE real_fps_3d_synced = %.0f", kFrames * 1000.0 / clock.elapsed());
    gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl.glDeleteFramebuffers(1, &fbo);
    gl.glDeleteTextures(1, &colorTex);
    gl.glDeleteRenderbuffers(1, &depthRbo);
  }

private:
  QString realSgy_;
  QList<QPair<QString, qint64>> sourceDirBefore_;
};

QTEST_MAIN(TestSeismicRealArea)
#include "tst_seismic_realarea.moc"
