// 性能闸门（wave/seismic-engine-deep 主线7）：
// tools/make_segy_fixture.py --mb 合成 ≥200MB 生产形状（ordinal 道字约定，
// 与 966MB 真工区同构）体；对 QuickOpen 首屏 / Dataset 直读首条剖面 /
// 时间片 / 任意折线剖面设定延迟预算。夹具生成一次后缓存在构建目录，
// 永不入库。真工区（966MB）验收走 docs/progress/seismic.md 的手动步骤。
#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>

#include <cmath>
#include <filesystem>

#include "Engine/QuickOpen.h"
#include "Engine/Sdk.h"

namespace {
constexpr double kQuickOpenBudgetMs = 5000.0;   // 秒级首屏（宽裕预算，覆盖共享机）
constexpr double kFirstSliceBudgetMs = 5000.0;  // 直读后端首条 inline 剖面
constexpr double kTimeSliceBudgetMs = 8000.0;   // 时间片（整面扫描，预算最宽）
constexpr double kSectionBudgetMs = 5000.0;     // 任意折线剖面（useReadPlan）
} // namespace

class TestSeismicPerf : public QObject
{
  Q_OBJECT

private:
  QString bigSgy_;

private slots:
  void initTestCase()
  {
    const QString perfDir = QStringLiteral(PALEO_SEISMIC_PERF_DIR);
    QVERIFY(QDir().mkpath(perfDir));
    bigSgy_ = perfDir + QStringLiteral("/perf_big.sgy");

    // 缺失或不足 200MB 时（重新）生成；生成是分钟级一次性成本，之后缓存
    const qint64 minBytes = 200ll * 1024 * 1024;
    if (QFileInfo(bigSgy_).size() < minBytes)
    {
      QElapsedTimer gen;
      gen.start();
      const int rc = QProcess::execute(
          QStringLiteral(PALEO_PYTHON3),
          {QStringLiteral(PALEO_SEGY_FIXTURE_TOOL),
           QStringLiteral("--out"), bigSgy_,
           QStringLiteral("--mb"), QStringLiteral("220")});
      QVERIFY2(rc == 0, "make_segy_fixture.py --mb 220 failed");
      qInfo("fixture generated in %.1f s", gen.elapsed() / 1000.0);
    }
    QVERIFY2(QFileInfo(bigSgy_).size() >= minBytes,
             "perf fixture must be >= 200 MB");
    qInfo("perf fixture: %s (%.1f MiB)", qPrintable(bigSgy_),
          QFileInfo(bigSgy_).size() / 1048576.0);
  }

  // QuickOpen 秒级首屏：规则探针（P5 ordinal 回退）+ 128 列真振幅预览
  void quickOpenFirstScreenBudget()
  {
    QElapsedTimer clock;
    clock.start();
    const auto quick = seismic::engine::QuickOpenSegyPreview(
        std::filesystem::path(bigSgy_.toStdString()), 128, nullptr);
    const double ms = clock.elapsed();
    qInfo("QuickOpen: %.0f ms (probe %0.f ms, firstRead %.0f ms), verified=%d",
          ms, quick.times.probeMs, quick.times.firstReadMs,
          quick.ruleVerified ? 1 : 0);
    QVERIFY2(quick.status.ok(), quick.status.message.c_str());
    QVERIFY2(quick.ruleVerified, quick.fallbackReason.c_str());
    QVERIFY(quick.columnsRead > 0);
    QVERIFY2(ms < kQuickOpenBudgetMs,
             qPrintable(QStringLiteral("QuickOpen %1 ms over budget %2")
                            .arg(ms).arg(kQuickOpenBudgetMs)));
  }

  // 直读后端首条 inline 剖面（冷缓存：.sgyidx 已由上一用例的探针流不产生，
  // Open 走规则/索引路径 + ReadInline 全 xline 读）
  void directFirstInlineSliceBudget()
  {
    namespace sdk = seismic::sdk;
    QElapsedTimer clock;
    seismic::engine::Status st;
    clock.start();
    auto ds = sdk::Dataset::Open(std::filesystem::path(bigSgy_.toStdString()),
                                 sdk::OpenOptions{}, st);
    QVERIFY2(ds != nullptr, st.message.c_str());
    const double openMs = clock.elapsed();

    const int midInline = ds->Metadata().inlineMin +
                          (ds->Metadata().inlineMax - ds->Metadata().inlineMin) / 2;
    clock.restart();
    seismic::engine::Slice2D slice;
    const auto status = ds->ReadInline(midInline, slice);
    const double sliceMs = clock.elapsed();
    qInfo("Open %0.f ms + ReadInline %0.f ms (%d cols x %d rows)",
          openMs, sliceMs, slice.width, slice.height);
    QVERIFY2(status.ok(), status.message.c_str());
    QVERIFY(slice.width > 100);
    QVERIFY2(openMs + sliceMs < kFirstSliceBudgetMs,
             qPrintable(QStringLiteral("open+slice %1 ms over budget %2")
                            .arg(openMs + sliceMs).arg(kFirstSliceBudgetMs)));
  }

  // 时间片整面（P2 mmap 并行路径）
  void timeSliceBudget()
  {
    namespace sdk = seismic::sdk;
    seismic::engine::Status st;
    auto ds = sdk::Dataset::Open(std::filesystem::path(bigSgy_.toStdString()),
                                 sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);
    const int midSample = ds->Metadata().sampleCount / 2;
    QElapsedTimer clock;
    clock.start();
    seismic::engine::Slice2D slice;
    const auto status = ds->ReadTimeSlice(midSample, slice);
    const double ms = clock.elapsed();
    qInfo("ReadTimeSlice: %.0f ms (%d x %d)", ms, slice.width, slice.height);
    QVERIFY2(status.ok(), status.message.c_str());
    QVERIFY2(ms < kTimeSliceBudgetMs,
             qPrintable(QStringLiteral("time slice %1 ms over budget %2")
                            .arg(ms).arg(kTimeSliceBudgetMs)));
  }

  // 任意折线剖面（useReadPlan 扇区合并读）
  void arbitrarySectionBudget()
  {
    namespace sdk = seismic::sdk;
    seismic::engine::Status st;
    auto ds = sdk::Dataset::Open(std::filesystem::path(bigSgy_.toStdString()),
                                 sdk::OpenOptions{}, st);
    QVERIFY(ds != nullptr);
    const auto &meta = ds->Metadata();
    seismic::engine::SectionRequest request;
    request.pathPoints = {
        {meta.inlineMin, meta.xlineMin},
        {(meta.inlineMin + meta.inlineMax) / 2, (meta.xlineMin + meta.xlineMax) / 2},
        {meta.inlineMax, meta.xlineMax}};
    request.useReadPlan = true;
    request.maxColumns = 2048;
    QElapsedTimer clock;
    clock.start();
    seismic::engine::Slice2D slice;
    const auto status = ds->ReadSection(request, slice);
    const double ms = clock.elapsed();
    qInfo("ReadSection: %.0f ms (%d cols, %llu ranges, %.2fx amp)",
          ms, slice.width,
          (unsigned long long)slice.planReadRanges, slice.planReadAmplification);
    QVERIFY2(status.ok(), status.message.c_str());
    QVERIFY(slice.width > 16);
    QVERIFY2(ms < kSectionBudgetMs,
             qPrintable(QStringLiteral("section %1 ms over budget %2")
                            .arg(ms).arg(kSectionBudgetMs)));
  }
};

QTEST_MAIN(TestSeismicPerf)
#include "tst_seismic_perf.moc"
