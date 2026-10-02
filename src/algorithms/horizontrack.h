// 层：数据
#pragma once

// horizontrack — 层位自动追踪核（纯数值，无 Qt/GIS 依赖）。
// 输入为平凡 float 缓冲：
//   · 2D 剖面 values[trace * nS + s]（道主序——一道垂直窗是内存连续段；
//     服务层从 SgySliceImage 行主序显式转置桥接）。
//   · 3D 体 values[((il * nXl) + xl) * nS + s]，与 engine::VoxelWindow 逐位同构。
// NaN = 缺失道/缺失采样：候选窗含 NaN → 相关按有效样本计，有效 < 4 记 0
// （该位置不可拾取）；缺失不外推填补（诚实失败——追踪失败区留空）。
//
// 算法（波形互相关主干，选型与因由见 .goal-loop-ledger-horizon-autotrack.md）：
//   · 模板 = 种子道窗（固定，抗漂移——归一化相关对位置不敏感，候选窗滑动
//     即可匹配大倾角同相轴）；倾角引导为隐式：搜索窗中心跟随前一道拾取
//     位置，半径 maxSearchSamples 即逐道最大跳跃/倾角约束。
//   · 归一化互相关（零均值 Pearson）= 匹配分与置信度的基。
//
// 置信度语义（钉死）：
//   conf = corr × coherence。corr = 接受位置种子模板与候选窗的 Pearson 相关
//   （负值钳 0），coherence = 同位置门数组值（未提供门数组时 = 1）。
//   降权信号各自独立单调：波形劣化 → corr 降；横向破碎 → coherence 降。
//   手动种子 conf = 1.0。
//   相干门语义：coherenceThreshold > 0 时，门值 < 阈值的候选不接受（硬停，
//   不降权放行）；门值 NaN = 无信息 → 弃权放行（相干体边界带 NaN 是缺失
//   不是不合格，不误杀边缘道）。

#include <functional>
#include <vector>

namespace paleo::hztrack
{

enum class StopReason
{
  Completed,        // 到达剖面边界（或体边界/限步长）
  CorrelationLoss,  // 最佳相关低于阈值（同相轴丢失/断层位移越界）
  CoherenceGate,    // 相关合格但候选被相干门槛尽数拒绝（断层判据）
  Cancelled,        // 取消谓词触发（部分结果如实返回）
  Invalid           // 输入几何/种子非法（结果为空）
};

struct TrackOptions
{
  int windowSamples = 24;            // 波形相关窗（垂直样点，钳到 [4, nS]）
  int maxSearchSamples = 12;         // 逐道搜索半径 = 最大跳跃/倾角约束
  double correlationThreshold = 0.6; // 低于阈值的道不接受并停追
  double coherenceThreshold = 0.0;   // 相干门（<= 0 = 关闭）
};

struct SeedPoint
{
  int trace = 0;   // 剖面内道序号；3D 扩散时为种子剖面内的 xl 序号
  int sample = 0;  // 窗口中心采样
};

struct TracedPick
{
  int trace = 0;
  int sample = 0;          // 窗口中心采样
  float confidence = 1.0f; // 语义见文件头；种子（手动）恒 1
};

struct TrackStop
{
  StopReason reason = StopReason::Completed;
  int trace = -1; // 停止处道序号（Cancelled/Invalid 无意义时 -1）
};

// 单种子双向追踪：picks 按 trace 升序（含种子本身），两端停因分开记录。
struct TrackResult
{
  std::vector<TracedPick> picks;
  TrackStop stopRight; // trace 递增方向
  TrackStop stopLeft;  // trace 递减方向
  bool valid() const { return !picks.empty(); }
};

// coherence：可选逐样点门数组（与 section 同布局 trace*nS+s，值域 [0,1]，
// 语义见文件头）；cancelled：逐道轮询的取消谓词（空 = 不可取消）。
TrackResult trackSection(const float *section, int nTraces, int nSamples,
                         SeedPoint seed, const TrackOptions &options,
                         const float *coherence = nullptr,
                         const std::function<bool()> &cancelled = nullptr);

// 多种子合并：逐结果按 trace 归并，同 trace 取高置信；并列取先出现者
// （稳定可重现）。输出按 trace 升序。
std::vector<TracedPick> mergeTraced(const std::vector<TrackResult> &results);

// ---- 3D 面扩散（最小可用：前沿扫掠）-------------------------------------------
// 种子剖面 seedIl 上的种子集先在该 IL 剖面内 2D 追踪 + 合并成前沿（trace =
// xl），再逐 IL 向两侧外推：新剖面每道以前沿剖面同 xl 道窗为模板、在原
// 拾取位置 ±maxSearchSamples 搜索。逐道跳跃天然 ≤ maxSearchSamples；离开
// 种子剖面的 IL 距离另受 maxInlineStep 约束（0 = 不限）。
// 诚实失败：某 xl 在某 IL 追丢后该列死亡，不再尝试更远 IL（不跨空外推）；
// 置信度 = 外推相关系数（3D 路径无相干门）。
struct VolumePick
{
  int il = 0;
  int xl = 0;
  int sample = 0;
  float confidence = 1.0f;
};

struct PropagateResult
{
  std::vector<VolumePick> picks; // (il, xl) 字典序升序
  int seedIl = 0;
  int ilMin = 0, ilMax = 0;      // 实际覆盖的 IL 范围（含种子剖面）
  StopReason stopReason = StopReason::Completed;
  bool valid() const { return !picks.empty(); }
};

PropagateResult propagateVolume(const float *volume, int nIl, int nXl, int nS,
                                int seedIl, const std::vector<SeedPoint> &seeds,
                                const TrackOptions &options,
                                int maxInlineStep = 0,
                                const std::function<bool()> &cancelled = nullptr);

} // namespace paleo::hztrack
