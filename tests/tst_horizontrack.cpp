// 层：测试壳（被测对象为数据层纯数值核 horizontrack）
#include <QtTest/QtTest>

#include "algorithms/horizontrack.h"
#include "algorithms/seismicattr.h"

#include <cmath>
#include <functional>
#include <limits>
#include <vector>

using namespace paleo::hztrack;

namespace
{

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();

// 确定性 LCG（测试噪声可复现，禁默认随机引擎）
struct Lcg
{
  quint32 s = 1u;
  float next01()
  {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) * (1.0f / 16777216.0f);
  }
};

// 合成剖面（道主序 trace*nS+s）：peakAt(trace) 处锥形余弦同相轴（半宽 14
// 样 ≥ 相关窗），noiseAmpAt(trace) 白噪（uniform [-amp, amp]，事件与背景
// 同加）。背景禁用平滑正弦——Pearson 相关尺度不变，周期近子波的正弦背景
// 相关可达 ~0.97，事件丢失后追踪会滑落跟随背景「假事件」（实测教训）。
// [nanFrom, nanTo) 道整道 NaN。
std::vector<float> makeSection(
    int nTraces, int nSamples, const std::function<int(int)> &peakAt,
    const std::function<float(int)> &noiseAmpAt, quint32 seed,
    int nanFrom = -1, int nanTo = -1)
{
  std::vector<float> v(static_cast<std::size_t>(nTraces) * nSamples, kNan);
  Lcg rng{seed};
  for (int t = 0; t < nTraces; ++t)
  {
    if (t >= nanFrom && t < nanTo)
      continue;
    const int peak = peakAt(t);
    const float amp = noiseAmpAt(t);
    for (int s = 0; s < nSamples; ++s)
    {
      const int d = s - peak;
      float val = std::abs(d) <= 14 ? std::cos(d / 14.0f * float(M_PI)) : 0.0f;
      if (amp > 0.0f)
        val += amp * (rng.next01() * 2.0f - 1.0f);
      v[static_cast<std::size_t>(t) * nSamples + s] = val;
    }
  }
  return v;
}

float zeroNoise(int) { return 0.0f; }

// 合成体（VoxelWindow 同构布局 [(il*nXl+xl)*nS+s]）：peak(il,xl) 同相轴面
std::vector<float> makeVolume(
    int nIl, int nXl, int nS, const std::function<int(int, int)> &peakAt)
{
  std::vector<float> v(static_cast<std::size_t>(nIl) * nXl * nS, kNan);
  for (int il = 0; il < nIl; ++il)
    for (int xl = 0; xl < nXl; ++xl)
    {
      const int peak = peakAt(il, xl);
      for (int s = 0; s < nS; ++s)
      {
        const int d = s - peak;
        float val = std::abs(d) <= 14 ? std::cos(d / 14.0f * float(M_PI)) : 0.0f;
        v[(static_cast<std::size_t>(il) * nXl + xl) * nS + s] = val;
      }
    }
  return v;
}

} // namespace

class TestHorizonTrack : public QObject
{
  Q_OBJECT

private slots:
  // ---- Oracle#1：已知倾角同相轴逐点对真值 ----
  void dippingEventFollowsTruth()
  {
    const int cols = 160, rows = 512;
    const auto peak = [](int t) { return 140 + 2 * t; };
    const std::vector<float> sec =
        makeSection(cols, rows, peak, zeroNoise, 3u);
    const TrackResult r = trackSection(sec.data(), cols, rows,
                                       {80, peak(80)}, {24, 12, 0.6});
    QVERIFY(r.valid());
    QCOMPARE(int(r.picks.size()), cols); // 全覆盖（无噪声干净合成）
    QCOMPARE(r.stopLeft.reason, StopReason::Completed);
    QCOMPARE(r.stopRight.reason, StopReason::Completed);
    for (const TracedPick &p : r.picks)
    {
      // 容差 ±1：拾取=相关窗中心，与锥形余弦峰位存在半样舍入
      QVERIFY(std::abs(p.sample - peak(p.trace)) <= 1);
      QVERIFY(p.confidence >= 0.6f);
      QVERIFY(p.confidence <= 1.0f);
    }
    QCOMPARE(r.picks.front().confidence, 1.0f); // 手动种子恒 1
  }

  // ---- Oracle#3：波形劣化 → 置信度单调下降（corr 路径）----
  void confidenceMonotonicUnderGradedNoise()
  {
    const int cols = 240, rows = 512;
    const auto peak = [](int t) { return 120 + t / 2; };
    // 4 区渐强噪声：均匀噪声 var=amp²/3 → 期望 corr ≈ √(σs²/(σs²+var))
    // ≈ 1.0/0.98/0.90/0.78（σs²≈0.52），单调降且全程 ≥ 阈值 0.5
    const auto amp = [](int t) {
      return t < 60 ? 0.0f : t < 120 ? 0.25f : t < 180 ? 0.6f : 1.0f;
    };
    const std::vector<float> sec =
        makeSection(cols, rows, peak, amp, 11u);
    const TrackResult r = trackSection(sec.data(), cols, rows,
                                       {30, peak(30)}, {24, 12, 0.5});
    QVERIFY(r.valid());
    QCOMPARE(int(r.picks.size()), cols); // 最噪区 corr≈0.78 仍全程可追
    double mean[4] = {0, 0, 0, 0};
    int cnt[4] = {0, 0, 0, 0};
    for (const TracedPick &p : r.picks)
    {
      const int z = p.trace / 60;
      mean[z] += p.confidence;
      ++cnt[z];
    }
    for (int z = 0; z < 4; ++z)
    {
      QCOMPARE(cnt[z], 60);
      mean[z] /= cnt[z];
    }
    QString diag;
    for (int z = 0; z < 4; ++z)
      diag += QString::number(mean[z], 'f', 3) + " ";
    qInfo("graded-noise mean conf: %s", qPrintable(diag));
    QVERIFY(mean[0] > mean[1]);
    QVERIFY(mean[1] > mean[2]);
    QVERIFY(mean[2] > mean[3]); // 单调语义：波形劣化必降权
  }

  // ---- Oracle#3：横向破碎（相干门值）→ 置信度乘法降权 ----
  void coherenceGateDownweightsConfidence()
  {
    const int cols = 240, rows = 512;
    const auto peak = [](int t) { return 120 + t / 2; };
    const std::vector<float> sec =
        makeSection(cols, rows, peak, zeroNoise, 5u);
    // 门数组分区 1.0/0.85/0.7/0.55（阈值 0.5 全放行，只验证乘法降权）
    std::vector<float> gate(static_cast<std::size_t>(cols) * rows, kNan);
    for (int t = 0; t < cols; ++t)
    {
      const float g = t < 60 ? 1.0f : t < 120 ? 0.85f : t < 180 ? 0.7f : 0.55f;
      for (int s = 0; s < rows; ++s)
        gate[static_cast<std::size_t>(t) * rows + s] = g;
    }
    TrackOptions opt{24, 12, 0.6, 0.5};
    const TrackResult r =
        trackSection(sec.data(), cols, rows, {30, peak(30)}, opt, gate.data());
    QVERIFY(r.valid());
    QCOMPARE(int(r.picks.size()), cols);
    double mean[4] = {0, 0, 0, 0};
    for (const TracedPick &p : r.picks)
      mean[p.trace / 60] += p.confidence;
    for (int z = 0; z < 4; ++z)
      mean[z] /= 60.0;
    QVERIFY(mean[0] > mean[1]);
    QVERIFY(mean[1] > mean[2]);
    QVERIFY(mean[2] > mean[3]); // conf = corr × coherence 单调语义
  }

  // ---- Oracle#1：断层位移越界 → 相关丢失停追（不穿越）----
  void faultDisplacementStopsTracking()
  {
    const int cols = 160, rows = 512, faultCol = 80, disp = 36;
    const auto peak = [faultCol, disp](int t) {
      return 140 + t / 2 + (t >= faultCol ? disp : 0);
    };
    const std::vector<float> sec =
        makeSection(cols, rows, peak, zeroNoise, 3u);
    // maxSearch=12 < disp=36：断层右盘事件在搜索半径外
    const TrackResult r = trackSection(sec.data(), cols, rows,
                                       {120, peak(120)}, {24, 12, 0.6});
    QVERIFY(r.valid());
    QCOMPARE(r.stopLeft.reason, StopReason::CorrelationLoss);
    // 断层盘边界在 79|80 之间（t>=80 为下盘+36）：右盘内部连续可追，
    // 跨面道 79 事件在搜索半径外 → 停在尝试失败的道 faultCol-1
    QCOMPARE(r.stopLeft.trace, faultCol - 1);
    QCOMPARE(r.stopRight.reason, StopReason::Completed);
    for (const TracedPick &p : r.picks)
      QVERIFY(p.trace >= faultCol); // 不穿越：断层左盘零拾取
    QVERIFY(int(r.picks.size()) >= cols - faultCol); // 右盘全程
  }

  // ---- Oracle#1：位移可被相关跟上时，相干门（复用 semblanceCoherence）
  //      仍把追踪挡在断层前 ----
  void faultStopsViaCoherenceGate()
  {
    const int cols = 160, rows = 512, faultCol = 80, disp = 16;
    const auto peak = [faultCol, disp](int t) {
      return 140 + t / 2 + (t >= faultCol ? disp : 0);
    };
    const std::vector<float> sec =
        makeSection(cols, rows, peak, zeroNoise, 3u);
    // 相干体：nIl=1 的伪体即剖面自身布局（trace=xl），xlHalf=1 三道侧向
    // semblance——断层处错动跨窗 → S 崩（复用属性核，不重写）
    std::vector<float> coh(static_cast<std::size_t>(cols) * rows, kNan);
    paleo::seisattr::semblanceCoherence(sec.data(), 1, cols, rows, 0, 1, 8,
                                        coh.data());
    const auto cohAt = [&](int t, int s) {
      return coh[static_cast<std::size_t>(t) * rows + s];
    };
    // 夹具自检：断层道事件位置相干 < 阈值，干净区 ≥ 0.8（保证断言不空转）
    QVERIFY(cohAt(faultCol, peak(faultCol)) < 0.5);
    QVERIFY(cohAt(40, peak(40)) > 0.8);

    // 无门：位移 16 ≤ maxSearch 20，相关能跨断层（对照）
    const TrackResult cross = trackSection(sec.data(), cols, rows,
                                           {120, peak(120)}, {24, 20, 0.6});
    QVERIFY(cross.valid());
    QVERIFY(cross.picks.front().trace < faultCol); // 对照组确实穿了

    // 有门：停在断层前
    TrackOptions opt{24, 20, 0.6, 0.55};
    const TrackResult r =
        trackSection(sec.data(), cols, rows, {120, peak(120)}, opt, coh.data());
    QVERIFY(r.valid());
    QCOMPARE(r.stopLeft.reason, StopReason::CoherenceGate);
    QVERIFY(r.stopLeft.trace >= faultCol - 1); // 就在断层处被挡
    QVERIFY(r.stopLeft.trace <= faultCol + 1);
    for (const TracedPick &p : r.picks)
      QVERIFY(p.trace >= faultCol); // 不穿越
  }

  // ---- Oracle#1/2：多种子合并语义（同道取高置信，并列取先入）----
  void multiSeedMergeSemantics()
  {
    // 语义单测：手工构造已知置信度
    TrackResult a, b;
    a.picks = {{0, 100, 0.5f}, {1, 100, 0.9f}, {2, 100, 0.5f}};
    b.picks = {{1, 101, 0.8f}, {2, 102, 0.5f}, {3, 103, 0.7f}};
    const std::vector<TracedPick> m = mergeTraced({a, b});
    QCOMPARE(int(m.size()), 4);
    QCOMPARE(m[0].trace, 0);
    QCOMPARE(m[0].confidence, 0.5f);
    QCOMPARE(m[1].trace, 1);
    QCOMPARE(m[1].confidence, 0.9f); // 0.9 > 0.8 取高
    QCOMPARE(m[2].trace, 2);
    QCOMPARE(m[2].confidence, 0.5f); // 并列 0.5=0.5 取先入（a）
    QCOMPARE(m[2].sample, 100);
    QCOMPARE(m[3].trace, 3);
    QCOMPARE(m[3].confidence, 0.7f);

    // 集成：干净剖面两端双种子 → 合并后每道恰一个拾取、全覆盖
    const int cols = 100, rows = 400;
    const auto peak = [](int t) { return 120 + t / 2; };
    const std::vector<float> sec =
        makeSection(cols, rows, peak, zeroNoise, 3u);
    const TrackResult r1 = trackSection(sec.data(), cols, rows,
                                        {10, peak(10)}, {24, 12, 0.6});
    const TrackResult r2 = trackSection(sec.data(), cols, rows,
                                        {90, peak(90)}, {24, 12, 0.6});
    const std::vector<TracedPick> merged = mergeTraced({r1, r2});
    QCOMPARE(int(merged.size()), cols);
    for (std::size_t i = 0; i < merged.size(); ++i)
      QCOMPARE(merged[i].trace, int(i)); // 升序无重复
  }

  // ---- Oracle#4：取消谓词逐道生效（部分结果如实返回）----
  void cancelInterruptsTracking()
  {
    const int cols = 300, rows = 400;
    const auto peak = [](int t) { return 120 + t / 2; };
    const std::vector<float> sec =
        makeSection(cols, rows, peak, zeroNoise, 3u);
    int calls = 0;
    const TrackResult r = trackSection(
        sec.data(), cols, rows, {150, peak(150)}, {24, 12, 0.6}, nullptr,
        [&calls]() { return ++calls > 20; });
    QCOMPARE(r.stopLeft.reason, StopReason::Cancelled);
    QCOMPARE(r.stopRight.reason, StopReason::Cancelled);
    QVERIFY(r.picks.size() > 1);
    QVERIFY(int(r.picks.size()) < cols); // 部分结果，非全程
    for (const TracedPick &p : r.picks) // 如实：仅在已确认的道上
      QVERIFY(std::abs(p.sample - peak(p.trace)) <= 1);
  }

  // ---- 诚实失败：NaN 道缺口即停，不跨空外推 ----
  void nanGapStopsHonestly()
  {
    const int cols = 160, rows = 400;
    const auto peak = [](int t) { return 120 + t / 2; };
    const std::vector<float> sec =
        makeSection(cols, rows, peak, zeroNoise, 3u, 80, 90);
    const TrackResult r = trackSection(sec.data(), cols, rows,
                                       {120, peak(120)}, {24, 12, 0.6});
    QVERIFY(r.valid());
    QCOMPARE(r.stopLeft.reason, StopReason::CorrelationLoss);
    QCOMPARE(r.stopLeft.trace, 89); // 缺口第一道即停
    for (const TracedPick &p : r.picks)
      QVERIFY(p.trace >= 90); // 缺口内及其左侧零拾取（事件仍在也不穿）
    QCOMPARE(r.stopRight.reason, StopReason::Completed);
  }

  // ---- 输入防线：非法种子/几何 → Invalid 空结果 ----
  void invalidInputRejected()
  {
    std::vector<float> sec(100 * 50, 0.0f);
    QCOMPARE(trackSection(sec.data(), 100, 50, {100, 10}, {}).stopLeft.reason,
             StopReason::Invalid);
    QCOMPARE(trackSection(sec.data(), 100, 50, {10, 500}, {}).stopRight.reason,
             StopReason::Invalid);
    QVERIFY(!trackSection(nullptr, 10, 10, {1, 1}, {}).valid());
    // 窗长钳制：剖面太薄 → Invalid；边角种子（窗缩）仍可追
    QCOMPARE(trackSection(sec.data(), 100, 3, {10, 1}, {}).stopLeft.reason,
             StopReason::Invalid);
    const TrackResult edge = trackSection(sec.data(), 100, 50, {10, 1}, {});
    QVERIFY(edge.valid());
  }

  // ---- Oracle#1(3D)：面扩散对解析同相轴面逐点收敛 ----
  void propagateVolumeMatchesAnalyticSurface()
  {
    const int nIl = 32, nXl = 48, nS = 384;
    const auto peak = [](int il, int xl) { return 100 + 3 * il + 2 * xl; };
    const std::vector<float> vol = makeVolume(nIl, nXl, nS, peak);
    const PropagateResult r = propagateVolume(
        vol.data(), nIl, nXl, nS, 16, {{24, peak(16, 24)}}, {24, 12, 0.6});
    QVERIFY(r.valid());
    QCOMPARE(int(r.picks.size()), nIl * nXl); // 全覆盖
    QCOMPARE(r.stopReason, StopReason::Completed);
    QCOMPARE(r.ilMin, 0);
    QCOMPARE(r.ilMax, nIl - 1);
    for (const VolumePick &p : r.picks)
    {
      QVERIFY(std::abs(p.sample - peak(p.il, p.xl)) <= 1); // 容差同 2D 因由
      QVERIFY(p.confidence >= 0.6f);
    }
  }

  // ---- 诚实失败(3D)：空块列死亡且不复生；限步长生效 ----
  void propagateVolumeHonestHolesAndStepLimit()
  {
    const int nIl = 32, nXl = 48, nS = 384;
    const auto peak = [](int il, int xl) { return 100 + 3 * il + 2 * xl; };
    std::vector<float> vol = makeVolume(nIl, nXl, nS, peak);
    // 空块：xl∈[20,26) × il∈[18,22) 整块 NaN
    for (int il = 18; il < 22; ++il)
      for (int xl = 20; xl < 26; ++xl)
        for (int s = 0; s < nS; ++s)
          vol[(static_cast<std::size_t>(il) * nXl + xl) * nS + s] = kNan;
    const PropagateResult r = propagateVolume(
        vol.data(), nIl, nXl, nS, 16, {{24, peak(16, 24)}}, {24, 12, 0.6});
    QVERIFY(r.valid());
    for (const VolumePick &p : r.picks)
    {
      const bool inHole = p.il >= 18 && p.il < 22 && p.xl >= 20 && p.xl < 26;
      QVERIFY2(!inHole, "空块内不得有拾取");
      // 死列不复生：空块列在 il≥18 后零拾取（il=22 起事件仍在也不跨空）
      if (p.xl >= 20 && p.xl < 26)
        QVERIFY(p.il < 18);
      else
        QVERIFY(std::abs(p.sample - peak(p.il, p.xl)) <= 1);
    }
    QCOMPARE(r.ilMax, nIl - 1); // 其余列仍扫到体边界
    // 限步长：maxInlineStep=5 → IL 覆盖 [11,21]
    const PropagateResult limited =
        propagateVolume(makeVolume(nIl, nXl, nS, peak).data(), nIl, nXl, nS,
                        16, {{24, peak(16, 24)}}, {24, 12, 0.6}, 5);
    QVERIFY(limited.valid());
    QCOMPARE(limited.ilMin, 16 - 5);
    QCOMPARE(limited.ilMax, 16 + 5);
  }

  // ---- Oracle#4(3D)：取消逐剖面生效 ----
  // #139：同相轴距剖面顶/底不足半窗（模板窗被钳位）时，拾取仍落在同相轴上，
  // 不被吸向窗中心（旧实现：事件在 3 → 全追到 12；在 96 → 92）。
  void edgeEventsNotPulledToWindowCentre()
  {
    const int nT = 20, nS = 100;
    for (const int ev : {3, 50, 96})
    {
      const std::vector<float> sec =
          makeSection(nT, nS, [ev](int) { return ev; }, zeroNoise, 1u);
      TrackOptions o;
      o.windowSamples = 24;
      o.maxSearchSamples = 12;
      o.correlationThreshold = 0.6;
      const TrackResult r = trackSection(sec.data(), nT, nS, {10, ev}, o);
      QCOMPARE(int(r.picks.size()), nT);
      for (const TracedPick &p : r.picks)
        QVERIFY2(p.sample == ev, qPrintable(QString("event %1 trace %2 picked %3")
                                                .arg(ev).arg(p.trace).arg(p.sample)));
    }
  }

  // #139：3D 前沿扩散同理（种子同相轴贴顶 3 样）。
  void propagateVolumeEdgeEventNotPulled()
  {
    const int nIl = 5, nXl = 5, nS = 100;
    for (const int ev : {3, 96})
    {
      const std::vector<float> vol =
          makeVolume(nIl, nXl, nS, [ev](int, int) { return ev; });
      TrackOptions o;
      o.windowSamples = 24;
      o.maxSearchSamples = 12;
      o.correlationThreshold = 0.6;
      const PropagateResult r = propagateVolume(vol.data(), nIl, nXl, nS, 2, {{2, ev}}, o);
      QCOMPARE(int(r.picks.size()), nIl * nXl);
      for (const auto &p : r.picks)
        QVERIFY2(p.sample == ev, qPrintable(QString("event %1 (%2,%3) picked %4")
                                                .arg(ev).arg(p.il).arg(p.xl).arg(p.sample)));
    }
  }

  void propagateVolumeCancelled()
  {
    const int nIl = 32, nXl = 48, nS = 384;
    const auto peak = [](int il, int xl) { return 100 + 3 * il + 2 * xl; };
    const std::vector<float> vol = makeVolume(nIl, nXl, nS, peak);
    int calls = 0;
    const PropagateResult r = propagateVolume(
        vol.data(), nIl, nXl, nS, 16, {{24, peak(16, 24)}}, {24, 12, 0.6}, 0,
        [&calls]() { return ++calls > 60; });
    QCOMPARE(r.stopReason, StopReason::Cancelled);
    QVERIFY(int(r.picks.size()) >= nXl); // 种子剖面已产出
    QVERIFY(int(r.picks.size()) < nIl * nXl); // 未全程
  }
};

QTEST_GUILESS_MAIN(TestHorizonTrack)
#include "tst_horizontrack.moc"
