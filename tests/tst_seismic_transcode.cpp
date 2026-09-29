// 层：测试壳
// P5 Phase 1 转码链路测试（D1.1–D1.10）：
// 分阶段聚合进度单调、断点探测三态、并行编码一致性、质量报告字段、坏道跳过、
// 同输出互斥、取消无假完成态（Auto 不升级）、坏 meta 识别与重建、
// 自适应金字塔层数、结构化日志可解析、续跑跳块、.partial 探测、scanning 阶段。
#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <QThread>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <vector>

#include "Engine/PagedPipeline.h"
#include "Engine/Sdk.h"
#include "Engine/TranscodeJob.h"
#include "Engine/WorkspaceFormat.h"

#include "domain/seismic/sgyvolume.h"
#include "services/paleotaskservice.h"
#include "services/seismictaskservice.h"

namespace {

// 标准 INLINE@189/CROSSLINE@193 字位的合成 SEG-Y（IEEE float）。
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
  const qint64 expected = 3600 + static_cast<qint64>(inlines) * xlines * (240 + ns * 4);
  return QFileInfo(filePath).size() == expected;
}

std::filesystem::path toPath(const QString &s)
{
  return std::filesystem::path(s.toStdString());
}

// 消息捕获器：结构化转码日志（D1.10）断言用。
class LogCollector
{
public:
  static void handler(QtMsgType, const QMessageLogContext &, const QString &msg)
  {
    if (msg.startsWith(QLatin1String("PALEO-SEISMIC-TRANSCODE ")))
      self()->lines << msg.mid(strlen("PALEO-SEISMIC-TRANSCODE ")).trimmed();
  }

  void install()
  {
    previous = qInstallMessageHandler(&LogCollector::handler);
  }

  ~LogCollector()
  {
    qInstallMessageHandler(previous);
    LogCollector::s_instance = nullptr;
  }

  QStringList lines;
  static inline LogCollector *s_instance = nullptr;

private:
  static LogCollector *self() { return s_instance; }
  QtMessageHandler previous = nullptr;
};

class Fixture : public QObject
{
public:
  Fixture() : seismic(&tasks, 16, this) { LogCollector::s_instance = &logs; }

  bool waitFor(const std::function<bool()> &done, int timeoutMs = 30000)
  {
    QElapsedTimer clock;
    clock.start();
    while (!done())
    {
      if (clock.elapsed() > timeoutMs)
        return false;
      QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
      QThread::msleep(5);
    }
    return true;
  }

  PaleoTaskService tasks;
  seismic::SeismicTaskService seismic;
  LogCollector logs;
};

} // namespace

using namespace seismic;

class TestSeismicTranscode : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase()
  {
    QVERIFY(writeStandardSegy(sgy(), 6, 5, 64));
  }

  // ---- D1.2/D1.6/D1.8：断点探测三态 ----
  void probeStatesLifecycle()
  {
    Fixture fix;
    QTemporaryDir dir;
    const QString p = dir.filePath("lifecycle.sgy");
    QVERIFY(writeStandardSegy(p, 6, 5, 512)); // ChunksS=8：中途取消必有半成品

    // 未开始：不存在
    seismic::SeismicWorkspaceProbe absent = fix.seismic.probeWorkspace(p);
    QVERIFY(!absent.exists);
    QCOMPARE(absent.stateText(), QStringLiteral("未开始"));

    // 引擎级中途取消（progress 返回 false）→ 半成品：resumable 且未 complete
    engine::CancelToken cancel;
    int seen = 0;
    const auto cancelled = engine::TranscodeSegyToWorkspace(
        toPath(p), toPath(p), engine::TranscodeOptions{}, &cancel,
        [&seen](const engine::TranscodeProgress &) { return ++seen < 3; });
    QCOMPARE(cancelled.status.code, engine::StatusCode::Cancelled);

    seismic::SeismicWorkspaceProbe half = fix.seismic.probeWorkspace(p);
    QVERIFY(half.exists);
    QVERIFY(half.readable);
    QVERIFY(!half.complete);
    QVERIFY(half.resumable);
    QVERIFY(half.chunksDone < half.chunksTotal);
    QVERIFY(half.chunksDone > 0);
    QVERIFY(half.formatVersion == 3);

    // D1.8：半成品 meta 不得被 Auto 当成品（假完成态）
    engine::Status st;
    auto ds = seismic::sdk::Dataset::Open(toPath(p), seismic::sdk::OpenOptions{}, st);
    QVERIFY(ds);
    QCOMPARE(QString::fromLatin1(ds->BackendName()), QStringLiteral("direct"));
    QVERIFY(ds->FellBackToDirect()); // 伴生 meta 存在但未就绪 → 如实标记回退

    // 续跑到完成 → complete
    const auto finished = engine::TranscodeSegyToWorkspace(
        toPath(p), toPath(p), engine::TranscodeOptions{}, nullptr, {});
    QVERIFY2(finished.status.ok(), finished.status.message.c_str());
    QVERIFY(finished.chunksSkipped > 0); // 续跑跳块证据
    seismic::SeismicWorkspaceProbe done = fix.seismic.probeWorkspace(p);
    QVERIFY(done.exists);
    QVERIFY(done.complete);
    QVERIFY(!done.resumable);

    // 完成后 Auto 升级工作区后端
    ds = seismic::sdk::Dataset::Open(toPath(p), seismic::sdk::OpenOptions{}, st);
    QVERIFY(ds);
    QCOMPARE(QString::fromLatin1(ds->BackendName()), QStringLiteral("workspace"));
    QVERIFY(!ds->FellBackToDirect());
  }

  // ---- D1.3：并行分片编码与单线程产物一致 ----
  void parallelEncodeMatchesSingleThread()
  {
    QTemporaryDir dir;
    const QString a = dir.filePath("a.sgy");
    const QString b = dir.filePath("b.sgy");
    QVERIFY(writeStandardSegy(a, 5, 5, 96));
    QVERIFY(writeStandardSegy(b, 5, 5, 96));

    engine::TranscodeOptions single;
    engine::TranscodeOptions parallel;
    parallel.writerThreads = 4;
#ifdef SEISMIC_HAVE_ZSTD
    // 真并行路径只在编码 codec 下激活（raw 自动退单线程）
    single.codec = engine::kCodecZstd;
    parallel.codec = engine::kCodecZstd;
#endif
    const auto ra = engine::TranscodeSegyToWorkspace(toPath(a), toPath(a), single, nullptr, {});
    const auto rb = engine::TranscodeSegyToWorkspace(toPath(b), toPath(b), parallel, nullptr, {});
    QVERIFY2(ra.status.ok(), ra.status.message.c_str());
    QVERIFY2(rb.status.ok(), rb.status.message.c_str());
    QCOMPARE(qint64(rb.chunksWritten), qint64(ra.chunksWritten));

    // 双方读同一条 inline 逐采样一致
    engine::Status st;
    auto da = seismic::sdk::Dataset::Open(toPath(a), seismic::sdk::OpenOptions{}, st);
    auto db = seismic::sdk::Dataset::Open(toPath(b), seismic::sdk::OpenOptions{}, st);
    QVERIFY(da && db);
    engine::Slice2D sa, sb;
    QVERIFY(da->ReadInline(1002, sa).ok());
    QVERIFY(db->ReadInline(1002, sb).ok());
    QCOMPARE(sa.width, sb.width);
    QCOMPARE(sa.height, sb.height);
    QCOMPARE(sa.values, sb.values);
  }

  // ---- D1.1：scanning 阶段真实发射 ----
  void scanningPhaseEmitted()
  {
    QTemporaryDir dir;
    const QString p = dir.filePath("scan.sgy");
    QVERIFY(writeStandardSegy(p, 4, 4, 64));
    QStringList phases;
    const auto r = engine::TranscodeSegyToWorkspace(
        toPath(p), toPath(p), engine::TranscodeOptions{}, nullptr,
        [&phases](const engine::TranscodeProgress &pr) {
          const QString phase = QString::fromStdString(pr.phase);
          if (phases.isEmpty() || phases.last() != phase)
            phases << phase;
          return true;
        });
    QVERIFY2(r.status.ok(), r.status.message.c_str());
    const QStringList expectedPhases{QStringLiteral("scanning"),
                                     QStringLiteral("transcoding"),
                                     QStringLiteral("finalizing")};
    QCOMPARE(phases, expectedPhases);
  }

  // ---- D1.4/D1.10/D1.1：服务级质量报告 + 结构化日志 + 聚合进度 ----
  void serviceReportAndLogs()
  {
    QTemporaryDir dir;
    const QString p = dir.filePath("rep.sgy");
    QVERIFY(writeStandardSegy(p, 5, 4, 64));

    Fixture fix;
    fix.logs.install();

    bool finished = false, reported = false;
    seismic::SeismicTranscodeReport report;
    int maxPercent = 0;
    int regressions = 0;
    int lastPercent = -1;

    PaleoTask *task = fix.seismic.startWorkspaceTranscodeDetailed(
        p, QString(),
        [&](bool ok, const QString &, const QString &) { finished = ok; },
        [&](const seismic::SeismicTranscodeReport &r) {
          report = r;
          reported = true;
        });
    QVERIFY(task != nullptr);
    QObject::connect(task, &PaleoTask::changed, [&]() {
      const int pct = task->percent();
      if (pct < lastPercent)
        ++regressions; // D1.1：全局百分比不得回退
      lastPercent = pct;
      maxPercent = std::max(maxPercent, pct);
    });
    QVERIFY(fix.waitFor([&]() { return finished && reported; }));
    QVERIFY(fix.logs.lines.isEmpty() == false);

    // 报告字段（D1.4）
    QVERIFY(report.ok);
    QCOMPARE(report.kind, QStringLiteral("sf3c"));
    QVERIFY(report.chunksTotal > 0);
    QCOMPARE(qint64(report.chunksWritten + report.chunksSkipped), report.chunksTotal);
    QVERIFY(report.tracesRead == 5 * 4);
    QCOMPARE(report.missingTraces, qint64(0));
    QCOMPARE(report.damagedTraces, qint64(0));
    QVERIFY(report.validValues);
    QVERIFY(report.valueMin < report.valueMax);
    QCOMPARE(int(report.coverage() * 100 + 0.5), 100);
    QCOMPARE(report.droppedRatio(), 0.0);

    // 聚合进度（D1.1）：达到高位且无回退
    QCOMPARE(regressions, 0);
    QVERIFY(maxPercent >= 99);

    // 结构化日志（D1.10）：start/finished 事件、JSON 可解析、字段齐
    QStringList events;
    for (const QString &line : std::as_const(fix.logs.lines))
    {
      QJsonParseError err{};
      const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &err);
      QCOMPARE(err.error, QJsonParseError::NoError);
      const QJsonObject obj = doc.object();
      QVERIFY(obj.contains("event"));
      QVERIFY(obj.contains("kind"));
      QVERIFY(obj.contains("output"));
      events << obj.value("event").toString();
    }
    QVERIFY(events.contains(QStringLiteral("start")));
    QVERIFY(events.contains(QStringLiteral("finished")));
    QVERIFY(!report.toJsonLine().isEmpty());
    QVERIFY(report.summaryLine().contains(QStringLiteral("覆盖")));
  }

  // ---- D1.5：缺席/损坏源道跳过 + 计数 + 值域 ----
  // 损坏道（ReadTrace IO 失败）与缺席道走同一 NaN 填充+计数管线；集成层用
  // 「缺席道」可确定性制造（清零一条道的 inline/xline 道字 → 网格中该槽无道），
  // IO 级损坏无法在临时目录里稳定伪造（截断会先炸索引收集）。
  void damagedTraceSkippedAndReported()
  {
    QTemporaryDir dir;
    const QString p = dir.filePath("damaged.sgy");
    QVERIFY(writeStandardSegy(p, 6, 4, 64));
    // 清零第 3 条道（inline=1000, xline=2003）的道字 → 该网格槽缺席
    QFile f(p);
    QVERIFY(f.open(QIODevice::ReadWrite));
    const qint64 traceOffset = 3600 + 3 * (240 + 64 * 4);
    QVERIFY(f.seek(traceOffset + 188));
    QVERIFY(f.write(QByteArray(8, 0)) == 8); // INLINE@189 + CROSSLINE@193
    f.close();

    engine::TranscodeOptions opts;
    const auto r = engine::TranscodeSegyToWorkspace(toPath(p), toPath(p), opts, nullptr, {});
    QVERIFY2(r.status.ok(), r.status.message.c_str()); // 缺席道不炸整体
    QCOMPARE(qint64(r.missingTraceCount), qint64(1));
    QCOMPARE(qint64(r.damagedTraceCount), qint64(0));
    QVERIFY(r.tracesRead == 23); // 6x4=24 格，缺席 1
    QVERIFY(std::isnan(r.valueMin) == false);

    // 值域只统计成功读入的道
    QVERIFY(r.valueMin < r.valueMax);

    // 缺席槽在产物中为 NaN（不冒充零振幅）
    engine::Status st;
    auto ds = seismic::sdk::Dataset::Open(toPath(p), seismic::sdk::OpenOptions{}, st);
    QVERIFY(ds);
    engine::Slice2D slice;
    QVERIFY(ds->ReadTimeSlice(10, slice).ok());
    // 被清零的道 3 = (inline 1000, xline 2003) → 6x4 时间片上恰一个 NaN 槽
    QCOMPARE(int(slice.width * slice.height), 24);
    int nanCount = 0;
    for (float v : slice.values)
      nanCount += std::isnan(v) ? 1 : 0;
    QCOMPARE(nanCount, 1);
    // 时间片布局：width=xline 轴（升序）、height=inline 轴按显示向降序
    // （fy = height-1-fi，与瓦片通道同约定）→ (inline 1000, xline 2003) =
    // 行 height-1、列 3
    QCOMPARE(int(slice.width), 4);
    QCOMPARE(int(slice.height), 6);
    const std::size_t expect =
        static_cast<std::size_t>(slice.height - 1) * slice.width + 3;
    QVERIFY(std::isnan(slice.values[expect]));
    QVERIFY(!std::isnan(slice.values[0])); // (inline 1005, xline 2000) 正常
  }

  // ---- D1.6：坏 meta（版本/损坏）识别 + 重转码自动重建 ----
  void corruptMetaDetectedAndRebuilt()
  {
    QTemporaryDir dir;
    const QString p = dir.filePath("corrupt.sgy");
    QVERIFY(writeStandardSegy(p, 4, 4, 64));
    const auto r = engine::TranscodeSegyToWorkspace(toPath(p), toPath(p),
                                                    engine::TranscodeOptions{}, nullptr, {});
    QVERIFY(r.status.ok());

    Fixture fix;
    // 探测：完整
    QVERIFY(fix.seismic.probeWorkspace(p).complete);
    // 写坏 meta
    {
      QFile meta(p + QStringLiteral(".sf3c.meta"));
      QVERIFY(meta.open(QIODevice::WriteOnly | QIODevice::Truncate));
      meta.write("corrupt");
      meta.close();
    }
    const seismic::SeismicWorkspaceProbe bad = fix.seismic.probeWorkspace(p);
    QVERIFY(bad.exists);
    QVERIFY(!bad.readable);
    QVERIFY(!bad.complete);
    QVERIFY(!bad.error.isEmpty());

    // 重转码：引擎 probe 失败 → 删除重建 → 成功
    const auto r2 = engine::TranscodeSegyToWorkspace(toPath(p), toPath(p),
                                                     engine::TranscodeOptions{}, nullptr, {});
    QVERIFY2(r2.status.ok(), r2.status.message.c_str());
    QVERIFY(fix.seismic.probeWorkspace(p).complete);
  }

  // ---- D1.7：同输出并发互斥 ----
  void mutualExclusionSameOutput()
  {
    QTemporaryDir dir;
    const QString p = dir.filePath("mutex.sgy");
    const QString l0 = dir.filePath("mutex.sf3p");
    QVERIFY(writeStandardSegy(p, 8, 8, 256)); // 够大让 L0 转码耗掉可观时间

    Fixture fix;
    bool firstDone = false;
    bool secondRejected = false;
    QString secondError;

    PaleoTask *first = fix.seismic.startPagedTranscodeDetailed(
        p, l0, /*buildLod=*/true,
        [&](bool, const QString &, const QString &) { firstDone = true; }, {});
    QVERIFY(first != nullptr);

    // 不进事件循环直接二次提交：互斥必须同步拒绝
    fix.seismic.startPagedTranscodeDetailed(
        p, l0, true,
        [&](bool ok, const QString &, const QString &err) {
          secondRejected = !ok;
          secondError = err;
        },
        {});
    QVERIFY(secondRejected);
    QVERIFY(secondError.contains(QStringLiteral("进行中")));

    QVERIFY(fix.waitFor([&]() { return firstDone; }));
    // 终态后互斥解除：可再次提交（续跑语义，立刻完成或复用）
    bool again = false;
    fix.seismic.startPagedTranscodeDetailed(
        p, l0, true,
        [&](bool ok, const QString &, const QString &) { again = ok; }, {});
    QVERIFY(fix.waitFor([&]() { return again; }));
  }

  // ---- D1.9：小体量自适应 → 不建 LOD；报告层数如实 ----
  void adaptiveLodSmallVolume()
  {
    QTemporaryDir dir;
    const QString p = dir.filePath("small.sgy");
    const QString l0 = dir.filePath("small.sf3p");
    QVERIFY(writeStandardSegy(p, 4, 4, 64)); // ~10KB 体量

    Fixture fix;
    bool done = false;
    seismic::SeismicTranscodeReport report;
    fix.seismic.startPagedTranscodeDetailed(
        p, l0, /*buildLod=*/true,
        [&](bool ok, const QString &, const QString &) { done = ok; },
        [&](const seismic::SeismicTranscodeReport &r) { report = r; });
    QVERIFY(fix.waitFor([&]() { return done; }));
    QVERIFY(report.ok);
    QVERIFY(report.lodLevels.isEmpty()); // <64MiB：不建金字塔
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("small.l1.sf3p"))));
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("small.l2.sf3p"))));

    // paged 探测：complete
    const seismic::SeismicWorkspaceProbe probe = fix.seismic.probePagedWorkspace(l0);
    QVERIFY(probe.exists);
    QVERIFY(probe.complete);
    QVERIFY(probe.readable);
  }

  // ---- D1.2：.partial 半成品探测 ----
  void pagedPartialProbeAfterCancel()
  {
    QTemporaryDir dir;
    const QString p = dir.filePath("pp.sgy");
    const QString l0 = dir.filePath("pp.sf3p");
    QVERIFY(writeStandardSegy(p, 6, 6, 256));

    engine::CancelToken cancel;
    int seen = 0;
    engine::PagedPipelineOptions opts;
    const auto r = engine::TranscodeSegyToPagedWorkspace(
        toPath(p), toPath(l0), opts, &cancel,
        [&seen](const engine::PagedPipelineProgress &) { return ++seen < 2; });
    QCOMPARE(r.status.code, engine::StatusCode::Cancelled);

    Fixture fix;
    const seismic::SeismicWorkspaceProbe probe = fix.seismic.probePagedWorkspace(l0);
    QVERIFY(probe.exists);
    QVERIFY(!probe.complete);
    QVERIFY(probe.resumable);

    // 续跑到完成
    const auto done = engine::BuildPagedPyramid(toPath(p), toPath(l0),
                                                engine::PagedPipelineOptions{}, nullptr, {});
    QVERIFY(done.status.ok());
    QVERIFY(fix.seismic.probePagedWorkspace(l0).complete);
  }

  // ---- D1.1（sf3p）：分阶段全局进度单调（服务级） ----
  void pagedAggregatedProgressMonotonic()
  {
    QTemporaryDir dir;
    const QString p = dir.filePath("agg.sgy");
    const QString l0 = dir.filePath("agg.sf3p");
    QVERIFY(writeStandardSegy(p, 8, 8, 512)); // 64 块以上，三阶段可观测

    Fixture fix;
    bool done = false;
    int regressions = 0;
    int lastPercent = -1;
    int maxPercent = 0;
    PaleoTask *task = fix.seismic.startPagedTranscodeDetailed(
        p, l0, /*buildLod=*/false,
        [&](bool, const QString &, const QString &) { done = true; }, {});
    QVERIFY(task);
    QObject::connect(task, &PaleoTask::changed, [&]() {
      const int pct = task->percent();
      if (pct < lastPercent)
        ++regressions;
      lastPercent = pct;
      maxPercent = std::max(maxPercent, pct);
    });
    QVERIFY(fix.waitFor([&]() { return done; }));
    QCOMPARE(regressions, 0); // 阶段切换不再归零回退
    QVERIFY(maxPercent >= 99);
  }

  // ---- D1.5（sf3p）：L0 缺席道计数 + NaN 填充不炸金字塔 ----
  void pagedDamagedTraceAccounting()
  {
    QTemporaryDir dir;
    const QString p = dir.filePath("pd.sgy");
    const QString l0 = dir.filePath("pd.sf3p");
    QVERIFY(writeStandardSegy(p, 4, 4, 128));
    QFile f(p);
    QVERIFY(f.open(QIODevice::ReadWrite));
    const qint64 traceOffset = 3600 + 5 * (240 + 128 * 4);
    QVERIFY(f.seek(traceOffset + 188));
    QVERIFY(f.write(QByteArray(8, 0)) == 8);
    f.close();

    const auto r = engine::BuildPagedPyramid(toPath(p), toPath(l0),
                                             engine::PagedPipelineOptions{}, nullptr, {});
    QVERIFY2(r.status.ok(), r.status.message.c_str());
    QCOMPARE(r.l0.missingTraceCount, 1ull);
    QCOMPARE(r.l0.damagedTraceCount, 0ull);
    QVERIFY(r.l0.tracesRead == 15);
    QVERIFY(!std::isnan(r.l0.valueMin));
    QVERIFY(r.l0.valueMin < r.l0.valueMax);
  }

private:
  static QString sgy()
  {
    static QString path = [] {
      QTemporaryDir dir;
      QDir().mkpath(QDir::temp().absoluteFilePath(QStringLiteral("paleo_tst_transcode")));
      return QDir::temp().absoluteFilePath(QStringLiteral("paleo_tst_transcode/main.sgy"));
    }();
    return path;
  }
};

QTEST_MAIN(TestSeismicTranscode)
#include "tst_seismic_transcode.moc"
