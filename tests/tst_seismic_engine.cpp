// Engine 矩阵测试（wave/seismic-engine-deep）：sdk::Dataset Open 矩阵（直读/
// .sf3c 工作区/Auto 回退/显式 .sf3p）、CancelToken 无半成品发布、paged 转码
// 取消-续跑-幂等、瓦片流、ReadVoxelWindow、渐进 LOD、useReadPlan 的
// interpolate 回落、P5 生产道字约定、POSIX StorageProfile。
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <tuple>
#include <vector>

#include "Engine/PagedPipeline.h"
#include "Engine/PagedWorkspace.h"
#include "Engine/QuickOpen.h"
#include "Engine/Sdk.h"
#include "Engine/StorageProfile.h"
#include "Engine/TranscodeJob.h"
#include "Engine/WorkspaceFormat.h"

#include "domain/seismic/sgyindexbuilder.h"
#include "domain/seismic/sgyindexcache.h"
#include "domain/seismic/sgyio.h"
#include "domain/seismic/sgyrulelayout.h"
#include "domain/seismic/sgyvolume.h"

namespace {

// 标准 INLINE@189/CROSSLINE@193 字位的合成 SEG-Y（IEEE float），尺寸可参数化。
bool writeStandardSegy(const QString &filePath, int inlines, int xlines, int ns,
                       int baseInline = 1000, int baseXline = 2000)
{
  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  QByteArray textHdr(3200, ' ');
  file.write(textHdr);
  QByteArray binHdr(400, 0);
  qToBigEndian<qint16>(static_cast<qint16>(xlines), reinterpret_cast<uchar *>(binHdr.data()) + 12);
  qToBigEndian<qint16>(2000, reinterpret_cast<uchar *>(binHdr.data()) + 16);
  qToBigEndian<qint16>(static_cast<qint16>(ns), reinterpret_cast<uchar *>(binHdr.data()) + 20);
  qToBigEndian<qint16>(5, reinterpret_cast<uchar *>(binHdr.data()) + 24);
  file.write(binHdr);
  const int traceCount = inlines * xlines;
  for (int i = 0; i < inlines; ++i)
  {
    for (int j = 0; j < xlines; ++j)
    {
      QByteArray trHdr(240, 0);
      qToBigEndian<qint32>(i * xlines + j + 1, reinterpret_cast<uchar *>(trHdr.data()) + 0);
      qToBigEndian<qint32>(baseInline + i, reinterpret_cast<uchar *>(trHdr.data()) + 188);
      qToBigEndian<qint32>(baseXline + j, reinterpret_cast<uchar *>(trHdr.data()) + 192);
      qToBigEndian<qint16>(static_cast<qint16>(ns), reinterpret_cast<uchar *>(trHdr.data()) + 114);
      file.write(trHdr);
      QByteArray samples(ns * 4, 0);
      for (int k = 0; k < ns; ++k)
      {
        const float val = static_cast<float>((i + 1) * 1000 + j * 10) + k * 0.25f;
        quint32 bits;
        std::memcpy(&bits, &val, 4);
        qToBigEndian<quint32>(bits, reinterpret_cast<uchar *>(samples.data()) + k * 4);
      }
      file.write(samples);
    }
  }
  file.close();
  const qint64 expected = 3600 + static_cast<qint64>(traceCount) * (240 + ns * 4);
  return QFileInfo(filePath).size() == expected;
}

float sampleOf(int i, int j, int k)
{
  return static_cast<float>((i + 1) * 1000 + j * 10) + k * 0.25f;
}

std::filesystem::path toPath(const QString &s)
{
  return std::filesystem::path(s.toStdString());
}

} // namespace

class TestSeismicEngine : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir tempDir_;
  QString standardSgy_;  // 24x24x128 标准字位合成体（转码/瓦片/体素主力）
  QString miniSgy_;      // 标准字位 12 道小体（Open 矩阵 / LOD）
  QString ordinalSgy_;   // 工区 ordinal 约定 fixture（P5 路径）

private slots:
  void initTestCase()
  {
    QVERIFY(tempDir_.isValid());
    standardSgy_ = tempDir_.filePath(QStringLiteral("standard_24x24.sgy"));
    QVERIFY(writeStandardSegy(standardSgy_, 24, 24, 128));
    miniSgy_ = tempDir_.filePath(QStringLiteral("mini_copy.sgy"));
    QVERIFY(QFile::copy(QStringLiteral(PROJECT_FIXTURE_DIR) + QStringLiteral("/mini_seismic.sgy"),
                        miniSgy_));
    ordinalSgy_ = QStringLiteral(SEGY_TESTDATA_DIR) + QStringLiteral("/segy/synthetic_4x5.sgy");
    QVERIFY(QFile::exists(ordinalSgy_));
  }

  // ---- Open 矩阵：直读 / .sf3c 工作区 / Auto 回退 / .meta 直传 ----
  void sdkOpenMatrixDirectWorkspaceAutoFallback()
  {
    namespace sdk = seismic::sdk;
    seismic::engine::Status st;

    auto ds = sdk::Dataset::Open(toPath(miniSgy_), sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);
    QVERIFY(st.ok());
    QCOMPARE(ds->BackendName(), "direct");
    QVERIFY(!ds->IsWorkspaceBackend());
    QVERIFY(!ds->FellBackToDirect());
    ds.reset();

    // 转码 .sf3c 后 Auto 自动升级工作区后端
    seismic::engine::TranscodeOptions options;
    const auto result = seismic::engine::TranscodeSegyToWorkspace(
        toPath(miniSgy_), toPath(miniSgy_), options, nullptr, {});
    QVERIFY2(result.status.ok(), result.status.message.c_str());
    QVERIFY(QFile::exists(miniSgy_ + QStringLiteral(".sf3c.meta")));

    ds = sdk::Dataset::Open(toPath(miniSgy_), sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);
    QCOMPARE(ds->BackendName(), "workspace");
    QVERIFY(!ds->FellBackToDirect());
    ds.reset();

    // 直接传 .meta 路径同样走工作区（Auto 接受伴生路径）
    ds = sdk::Dataset::Open(toPath(miniSgy_ + QStringLiteral(".sf3c.meta")),
                            sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);
    QCOMPARE(ds->BackendName(), "workspace");
    ds.reset();

    // 破坏 .meta：Auto 必须安全回落直读，而不是失败
    {
      QFile meta(miniSgy_ + QStringLiteral(".sf3c.meta"));
      QVERIFY(meta.open(QIODevice::WriteOnly));
      meta.write("corrupt");
      meta.close();
    }
    ds = sdk::Dataset::Open(toPath(miniSgy_), sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);
    QCOMPARE(ds->BackendName(), "direct");
    QVERIFY(ds->FellBackToDirect());
    // 回落直读后数据仍可读
    seismic::engine::Slice2D slice;
    QVERIFY(ds->ReadInline(1001, slice).ok());
    QCOMPARE(slice.width, 4);
    QCOMPARE(slice.height, 64);
  }

  // ---- Paged 矩阵：显式 .sf3p / Backend::Paged / Auto 永不自动升级 .sf3p ----
  void pagedOpenMatrixAndAutoNeverUpgrades()
  {
    namespace sdk = seismic::sdk;
    const QString l0 = miniSgy_ + QStringLiteral(".sf3p");
    seismic::engine::PagedPipelineOptions options;
    const auto pyramid = seismic::engine::BuildPagedPyramid(
        toPath(miniSgy_), toPath(l0), options, nullptr, {});
    QVERIFY2(pyramid.l0.status.ok(), pyramid.l0.status.message.c_str());
    QVERIFY(QFile::exists(l0));

    seismic::engine::Status st;
    // 显式 .sf3p 路径（Backend::Auto）
    auto ds = sdk::Dataset::Open(toPath(l0), sdk::OpenOptions{}, st);
    QVERIFY2(ds != nullptr, st.message.c_str());
    QVERIFY(ds->IsPagedBackend());
    QCOMPARE(ds->BackendName(), "paged-workspace");
    ds.reset();

    // Backend::Paged 显式枚举同样打开
    sdk::OpenOptions paged;
    paged.backend = sdk::Backend::Paged;
    ds = sdk::Dataset::Open(toPath(l0), paged, st);
    QVERIFY(ds != nullptr);
    QVERIFY(ds->IsPagedBackend());
    ds.reset();

    // Backend::Paged 指向 .sgy 本体：不是 paged 载体，必须明确失败
    ds = sdk::Dataset::Open(toPath(miniSgy_), paged, st);
    QVERIFY(ds == nullptr);
    QVERIFY(!st.ok());

    // Auto 语义差异：旁生 .sf3p 存在时 Auto 仍是直读（只发现 .sf3c.meta）
    ds = sdk::Dataset::Open(toPath(miniSgy_), sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);
    QCOMPARE(ds->BackendName(), "direct");
  }

  // ---- P5：生产 ordinal 道字约定（@188/@192 恒 0）走通引擎全链 ----
  void ordinalConventionP5Paths()
  {
    // QuickOpen：规则探针经 P5 回退字位成功并给出真振幅预览
    const auto quick = seismic::engine::QuickOpenSegyPreview(toPath(ordinalSgy_), 128, nullptr);
    QVERIFY2(quick.status.ok(), quick.status.message.c_str());
    QVERIFY2(quick.ruleVerified, quick.fallbackReason.c_str());
    QCOMPARE(quick.metadata.inlineMin, 10);
    QCOMPARE(quick.metadata.inlineMax, 13);
    QCOMPARE(quick.metadata.xlineMin, 100);
    QCOMPARE(quick.metadata.xlineMax, 104);
    QCOMPARE(quick.preview.width, 5);
    QCOMPARE(quick.columnsRead, 5);
    // 预览选中央道序(traceCount/2=10 -> i=2, inline 12)：row 0 是最深采样 s=63 -> 300.63
    QCOMPARE(quick.selectedInline, 12);
    QVERIFY(quick.preview.Valid(0, 0));
    QCOMPARE(std::round(quick.preview.Value(0, 0) * 100.0f) / 100.0f, 300.63f);

    // 全卷索引：inline/crossline 取自 field record@8 / CDP@20
    seismic::SgyIndexPtr index;
    std::string err;
    QVERIFY(seismic::SgyIndexBuilder::Build(toPath(ordinalSgy_), index, err));
    QVERIFY(index->complete);
    QCOMPARE(index->inlineMin, 10);
    QCOMPARE(index->xlineMax, 104);
    QCOMPARE(index->FindTraceIndex(11, 102), 7);

    // sdk 直读 + 精确采样值（value = (i+1)*100 + j + k/100）
    namespace sdk = seismic::sdk;
    seismic::engine::Status st;
    auto ds = sdk::Dataset::Open(toPath(ordinalSgy_), sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);
    seismic::engine::TraceData trace;
    QVERIFY(ds->ReadTrace(11, 101, trace).ok());
    QCOMPARE(trace.sampleCount, 64);
    QCOMPARE(trace.samples[0], 201.0f);
    QCOMPARE(std::round(trace.samples[63] * 100.0f) / 100.0f, 201.63f);

    seismic::engine::Slice2D slice;
    QVERIFY(ds->ReadInline(11, slice).ok());
    QCOMPARE(slice.width, 5);
    QCOMPARE(slice.height, 64);
    QCOMPARE(std::round(slice.values[0] * 100.0f) / 100.0f, 200.63f);
  }

  // ---- QuickOpen 取消与坏文件 ----
  void quickOpenCancelAndGarbage()
  {
    seismic::engine::CancelToken cancel;
    cancel.Cancel();
    const auto cancelled = seismic::engine::QuickOpenSegyPreview(
        toPath(ordinalSgy_), 128, &cancel);
    QCOMPARE(cancelled.status.code, seismic::engine::StatusCode::Cancelled);

    const auto missing = seismic::engine::QuickOpenSegyPreview(
        toPath(tempDir_.filePath(QStringLiteral("nope.sgy"))), 128, nullptr);
    QVERIFY(!missing.status.ok());
    QVERIFY(!missing.ruleVerified);
  }

  // ---- paged 转码：取消留 .partial → 续跑跳块 → 再跑幂等 reuse ----
  void pagedBuildCancelResumeIdempotent()
  {
    const QString l0 = tempDir_.filePath(QStringLiteral("paged_resume.sf3p"));
    seismic::engine::PagedPipelineOptions options;

    // 第一遍：l0 阶段写出首块后取消
    int progressCalls = 0;
    auto cancelEarly = [&progressCalls](const seismic::engine::PagedPipelineProgress &p) {
      ++progressCalls;
      return !(p.phase == "l0-transcode" && p.chunksDone >= 1);
    };
    const auto cancelled = seismic::engine::BuildPagedPyramid(
        toPath(standardSgy_), toPath(l0), options, nullptr, cancelEarly);
    QCOMPARE(cancelled.l0.status.code, seismic::engine::StatusCode::Cancelled);
    QVERIFY(progressCalls > 0);
    QVERIFY(QFile::exists(l0 + QStringLiteral(".partial")));
    QVERIFY(!QFile::exists(l0)); // 取消不发布成品

    // 第二遍：续跑完成，已写块被跳过
    std::uint64_t skippedSeen = 0;
    auto watch = [&skippedSeen](const seismic::engine::PagedPipelineProgress &p) {
      skippedSeen = std::max(skippedSeen, p.chunksSkipped);
      return true;
    };
    const auto resumed = seismic::engine::BuildPagedPyramid(
        toPath(standardSgy_), toPath(l0), options, nullptr, watch);
    QVERIFY2(resumed.l0.status.ok(), resumed.l0.status.message.c_str());
    QVERIFY(resumed.l0.chunksWritten > 0);
    QCOMPARE(resumed.l1.status.ok(), true);
    QCOMPARE(resumed.l2.status.ok(), true);
    QVERIFY(skippedSeen > 0);

    // 第三遍：完整成品 → 幂等 reuse，不重写
    const auto again = seismic::engine::BuildPagedPyramid(
        toPath(standardSgy_), toPath(l0), options, nullptr, {});
    QVERIFY(again.l0.status.ok());
    QVERIFY(again.l0.reused);
  }

  // ---- .sf3c 转码：取消是可续跑状态（引擎按设计发布续跑元数据）→ 续跑完成 → Auto 稳定升级 ----
  void sf3cTranscodeCancelIsResumable()
  {
    const QString sgy = tempDir_.filePath(QStringLiteral("sf3c_cancel.sgy"));
    QVERIFY(writeStandardSegy(sgy, 96, 96, 128)); // 8 个 64³ 块，取消可稳定命中中途
    seismic::engine::TranscodeOptions options;
    auto cancelEarly = [](const seismic::engine::TranscodeProgress &p) {
      return !(p.phase == "transcoding" && p.chunksDone >= 1);
    };
    const auto cancelled = seismic::engine::TranscodeSegyToWorkspace(
        toPath(sgy), toPath(sgy), options, nullptr, cancelEarly);
    QCOMPARE(cancelled.status.code, seismic::engine::StatusCode::Cancelled);

    // 续跑完成：跳过已写分片并发布完整工作区
    const auto done = seismic::engine::TranscodeSegyToWorkspace(
        toPath(sgy), toPath(sgy), options, nullptr, {});
    QVERIFY2(done.status.ok(), done.status.message.c_str());
    QVERIFY(done.chunksSkipped > 0);
    QVERIFY(QFile::exists(sgy + QStringLiteral(".sf3c.meta")));

    // 完成后 Auto 才稳定升级为工作区后端且数据正确
    namespace sdk = seismic::sdk;
    seismic::engine::Status st;
    auto ds = sdk::Dataset::Open(toPath(sgy), sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);
    QCOMPARE(ds->BackendName(), "workspace");
    seismic::engine::Slice2D slice;
    QVERIFY(ds->ReadInline(1002, slice).ok());
    QCOMPARE(slice.width, 96);
    QCOMPARE(std::round(slice.values[0] * 100.0f) / 100.0f,
             std::round((sampleOf(2, 0, 127)) * 100.0f) / 100.0f);
  }

  // ---- 索引构建取消：无半成品（引擎侧）----
  void indexScanCancelIsClean()
  {
    const QString sgy = tempDir_.filePath(QStringLiteral("cancel_index.sgy"));
    QVERIFY(writeStandardSegy(sgy, 24, 24, 128));
    auto progress = [](int processed, int) { return processed < 100; };
    seismic::SgyIndexPtr index;
    std::string err;
    QVERIFY(!seismic::SgyIndexBuilder::Build(toPath(sgy), index, err, progress));
    QVERIFY(index == nullptr); // 取消不发布索引
    QVERIFY(!QFile::exists(sgy + QStringLiteral(".sgyidx")));
  }

  // ---- 瓦片流：焦点优先 + 与整面读数逐值一致 ----
  void timeSliceTiledStreamMatchesFull()
  {
    const QString l0 = tempDir_.filePath(QStringLiteral("paged_tiles.sf3p"));
    seismic::engine::PagedPipelineOptions options;
    options.buildLod1 = false;
    options.buildLod2 = false;
    const auto built = seismic::engine::BuildPagedPyramid(
        toPath(standardSgy_), toPath(l0), options, nullptr, {});
    QVERIFY2(built.l0.status.ok(), built.l0.status.message.c_str());

    namespace sdk = seismic::sdk;
    seismic::engine::Status st;
    auto ds = sdk::Dataset::Open(toPath(l0), sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);

    const int sample = 64;
    const int focusX = 12; // 网格中心 xline 索引
    const int focusY = 11; // 引擎把 inline 焦点翻到显示向行坐标：fy = height-1-fi
    const int tileSize = 8;
    std::vector<std::tuple<int, int, int, int>> seen; // x, y, completed, total
    long long lastDistance = -1;
    bool focusOrderOk = true;
    seismic::engine::Slice2D tiled;
    // 与引擎同口径：按「焦点到瓦片矩形」的距离验收焦点优先序
    const auto publish = [&seen, &lastDistance, &focusOrderOk, focusX, focusY, tileSize](
                             int x, int y, seismic::engine::Slice2D &&, int completed, int total) {
      const auto rectDistance = [](int lo, int hi, int focus) {
        return static_cast<long long>(std::clamp(focus, lo, hi) - focus) *
               static_cast<long long>(std::clamp(focus, lo, hi) - focus);
      };
      const long long distance = rectDistance(x, x + tileSize - 1, focusX) +
                                 rectDistance(y, y + tileSize - 1, focusY);
      if (distance < lastDistance)
        focusOrderOk = false;
      lastDistance = distance;
      seen.emplace_back(x, y, completed, total);
      return true;
    };
    // 引擎侧有效焦点 = (fx=12, fy=23-11=12)?? —— fy=height-1-fi，故传 inline 值 1012(fi=12)
    // 后 fy=11；与 publish 断言用的 (focusX=12, focusY=11) 一致。
    const auto status = ds->ReadTimeSliceTiled(sample, 8, 1012, 2012,
                                               publish, tiled, nullptr);
    QVERIFY2(status.ok(), status.message.c_str());
    QVERIFY2(focusOrderOk, "tiles must be published focus-first");
    QCOMPARE(tiled.width, 24);
    QCOMPARE(tiled.height, 24);
    QVERIFY(!seen.empty());
    QCOMPARE(std::get<2>(seen.back()), static_cast<int>(seen.size()));
    QCOMPARE(std::get<3>(seen.back()), static_cast<int>(seen.size()));

    seismic::engine::Slice2D full;
    QVERIFY(ds->ReadTimeSlice(sample, full).ok());
    QCOMPARE(full.width, tiled.width);
    QCOMPARE(full.height, tiled.height);
    QCOMPARE(static_cast<int>(full.values.size()), static_cast<int>(tiled.values.size()));
    for (std::size_t i = 0; i < full.values.size(); ++i)
    {
      const bool nanA = std::isnan(full.values[i]);
      const bool nanB = std::isnan(tiled.values[i]);
      QCOMPARE(nanA, nanB);
      if (!nanA)
        QCOMPARE(full.values[i], tiled.values[i]);
    }

    // 取消：publish 返回 false → 引擎按取消收场，不发布结果
    seismic::engine::Slice2D cancelled;
    const auto cancelledStatus = ds->ReadTimeSliceTiled(
        sample, 8, 1012, 2012,
        [](int, int, seismic::engine::Slice2D &&, int, int) { return false; },
        cancelled, nullptr);
    QCOMPARE(cancelledStatus.code, seismic::engine::StatusCode::Cancelled);
  }

  // ---- ReadVoxelWindow：窗口取数与逐道读数一致 ----
  void voxelWindowMatchesTraceReads()
  {
    const QString l0 = tempDir_.filePath(QStringLiteral("paged_voxel.sf3p"));
    seismic::engine::PagedPipelineOptions options;
    options.buildLod1 = false;
    options.buildLod2 = false;
    const auto built = seismic::engine::BuildPagedPyramid(
        toPath(standardSgy_), toPath(l0), options, nullptr, {});
    QVERIFY2(built.l0.status.ok(), built.l0.status.message.c_str());

    namespace sdk = seismic::sdk;
    seismic::engine::Status st;
    auto ds = sdk::Dataset::Open(toPath(l0), sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);

    seismic::engine::VoxelWindowRequest req;
    req.inlineBegin = 1002; // i=2
    req.xlineBegin = 2003;  // j=3
    req.sampleBegin = 10;
    req.inlineCount = 3;
    req.xlineCount = 2;
    req.sampleCount = 5;
    seismic::engine::VoxelWindow window;
    const auto status = ds->ReadVoxelWindow(req, window);
    QVERIFY2(status.ok(), status.message.c_str());
    QCOMPARE(window.box.inlineCount, 3);
    QCOMPARE(window.box.xlineCount, 2);
    QCOMPARE(window.box.sampleCount, 5);
    QCOMPARE(static_cast<int>(window.values.size()), 3 * 2 * 5);

    for (int di = 0; di < 3; ++di)
    {
      for (int dj = 0; dj < 2; ++dj)
      {
        seismic::engine::TraceData trace;
        QVERIFY2(ds->ReadTrace(req.inlineBegin + di, req.xlineBegin + dj, trace).ok(),
                 "trace read inside the window");
        for (int dk = 0; dk < 5; ++dk)
        {
          const float expected = trace.samples[req.sampleBegin + dk];
          const float actual = window.values[
              (static_cast<std::size_t>(di) * 2 + dj) * 5 + dk];
          QCOMPARE(actual, expected);
        }
      }
    }

    // 越界窗口：起点不在轴上 → NotFound（不夹取不伪造）
    seismic::engine::VoxelWindowRequest bad = req;
    bad.inlineBegin = 999999;
    seismic::engine::VoxelWindow badWindow;
    QCOMPARE(ds->ReadVoxelWindow(bad, badWindow).code,
             seismic::engine::StatusCode::NotFound);
  }

  // ---- 渐进 LOD：发现兄弟层级 → 粗开 → SetActiveLod 精化 ----
  void progressiveLodDiscoveryAndSwitch()
  {
    const QString l0 = miniSgy_ + QStringLiteral(".sf3p"); // pagedOpenMatrix 已产 LOD 兄弟文件
    QVERIFY(QFile::exists(l0));
    QVERIFY(QFile::exists(miniSgy_ + QStringLiteral(".l1.sf3p")));
    QVERIFY(QFile::exists(miniSgy_ + QStringLiteral(".l2.sf3p")));

    namespace sdk = seismic::sdk;
    sdk::OpenOptions options;
    options.progressiveLod = true;
    seismic::engine::Status st;
    auto ds = sdk::Dataset::Open(toPath(l0), options, st);
    QVERIFY2(ds != nullptr, st.message.c_str());
    QCOMPARE(ds->LodLevelCount(), 2); // 较粗层 {L1, L2}；L0 是基座不计入
    QCOMPARE(ds->LodLevelAt(0), 1);
    QCOMPARE(ds->LodLevelAt(1), 2);
    QCOMPARE(ds->ActiveLodLevel(), 2); // 从最粗层开
    QVERIFY(QString::fromStdString(ds->QualityName()).startsWith(QLatin1String("L2")));

    seismic::engine::Slice2D coarse;
    QVERIFY(ds->ReadInline(1001, coarse).ok());
    QCOMPARE(coarse.width, 1); // 4 xline / L2 因子 8 -> 1 列

    QVERIFY(ds->SetActiveLod(0).ok());
    QCOMPARE(ds->ActiveLodLevel(), 0);
    QVERIFY(QString::fromStdString(ds->QualityName()).startsWith(QLatin1String("L0")));
    seismic::engine::Slice2D fine;
    QVERIFY(ds->ReadInline(1001, fine).ok());
    QCOMPARE(fine.width, 4);

    const auto unknown = ds->SetActiveLod(7);
    QCOMPARE(unknown.code, seismic::engine::StatusCode::InvalidArgument);
  }

  // ---- useReadPlan + interpolate：静默回落 legacy builder，结果一致 ----
  void readPlanInterpolateFallsBackToLegacy()
  {
    const QString sgy = tempDir_.filePath(QStringLiteral("section_interp.sgy"));
    QVERIFY(writeStandardSegy(sgy, 24, 24, 128));

    namespace sdk = seismic::sdk;
    seismic::engine::Status st;
    auto ds = sdk::Dataset::Open(toPath(sgy), sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);

    auto makeRequest = [] {
      seismic::engine::SectionRequest request;
      request.pathPoints = { { 1000, 2000 }, { 1012, 2012 }, { 1023, 2023 } };
      request.interpolate = true; // 关键：planned 路径仅支持 nearest-trace
      request.maxColumns = 2048;
      return request;
    };
    seismic::engine::Slice2D planned;
    {
      seismic::engine::SectionRequest request = makeRequest();
      request.useReadPlan = true;
      const auto status = ds->ReadSection(request, planned);
      QVERIFY2(status.ok(), status.message.c_str());
    }
    seismic::engine::Slice2D legacy;
    {
      seismic::engine::SectionRequest request = makeRequest();
      request.useReadPlan = false;
      const auto status = ds->ReadSection(request, legacy);
      QVERIFY2(status.ok(), status.message.c_str());
    }
    QCOMPARE(planned.width, legacy.width);
    QCOMPARE(planned.height, legacy.height);
    QCOMPARE(static_cast<int>(planned.values.size()),
             static_cast<int>(legacy.values.size()));
    for (std::size_t i = 0; i < planned.values.size(); ++i)
      QCOMPARE(planned.values[i], legacy.values[i]);
    QVERIFY(planned.width > 3); // 插值列确实生效（非 3 个角点列）
  }

  // ---- POSIX StorageProfile：探测不崩、描述可用 ----
  void storageProfilePosixProbe()
  {
    const auto storageClass = seismic::engine::ClassifyPath(
        toPath(tempDir_.path()));
    QVERIFY(storageClass == seismic::engine::StorageClass::Unknown ||
            storageClass == seismic::engine::StorageClass::Rotational ||
            storageClass == seismic::engine::StorageClass::SataSsd ||
            storageClass == seismic::engine::StorageClass::Nvme);
    QVERIFY(strlen(seismic::engine::StorageClassName(storageClass)) > 0);

    const auto profiles = seismic::engine::ResolveStorageProfiles(
        toPath(standardSgy_), toPath(tempDir_.path()), toPath(tempDir_.path()));
    QVERIFY(!profiles.sourcePath.empty());
    QVERIFY(profiles.source.sequentialBlockBytes > 0);
    QVERIFY(!profiles.describe().empty());

    const auto hdd = seismic::engine::ProfileFor(seismic::engine::StorageClass::Rotational, "");
    QCOMPARE(hdd.readQueueDepth, 1u);
    const auto badOverride = seismic::engine::ProfileFor(
        seismic::engine::StorageClass::Unknown, "nonsense");
    QCOMPARE(badOverride.storageClass, seismic::engine::StorageClass::Unknown);
  }

  // ---- .sgyidx 伴生：ordinal fixture 经引擎索引后伴生可带走 ----
  void companionSgyidxOnOrdinalFixture()
  {
    const QString sgy = tempDir_.filePath(QStringLiteral("companion.sgy"));
    QVERIFY(QFile::copy(ordinalSgy_, sgy));
    seismic::SgyIndexPtr index;
    std::string err;
    QVERIFY(seismic::SgyIndexBuilder::Build(toPath(sgy), index, err));
    // P1 语义：显式落盘为伴生 <name>.sgyidx，工区搬机器可直接带走
    QVERIFY(seismic::SgyIndexCache::Save(index, err, toPath(sgy + QStringLiteral(".sgyidx"))));
    QVERIFY(QFile::exists(sgy + QStringLiteral(".sgyidx")));
    std::string reason;
    const auto loaded = seismic::SgyIndexCache::Load(toPath(sgy), reason);
    QVERIFY2(loaded != nullptr, reason.c_str());
    QVERIFY(loaded->complete);
    QCOMPARE(loaded->inlineMin, 10);
  }
};

QTEST_MAIN(TestSeismicEngine)
#include "tst_seismic_engine.moc"
