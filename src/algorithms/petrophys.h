// 层：数据
#pragma once

// petrophys — 测井岩石物理公式核 + 深度对齐 + 曲线 QC（纯数值，无 Qt/GIS 依赖）。
// 输入输出均为平凡 double 缓冲（与 LasCurve::values 同元素类型），逐点运算；
// NaN 语义：任一输入 NaN → 该输出位 NaN（诚实传播，不补值）；
// 除零/域外按 IEEE 与显式域约定如实输出，异常标记交给 QC 面，算子内不吞。
// 服务层（PetroPhysTaskService 批处理）负责分井/进度/取消/落盘，核函数不掺和。
//
// 公式引用（地质约定，禁臆造；缩写 AK04 = Asquith & Krygowski, "Basic Well
// Log Analysis", 2nd ed., AAPG Methods in Exploration 33, 2004）：
//   · Vsh 线性（GR 伽马指数基线）：AK04 ch.4。
//   · Vsh Larionov 年轻岩（第三系）/老岩（古生代）：Larionov 1969（苏联
//     原文，转引 AK04 ch.4 非线性关系）。
//   · Vsh Clavier：Clavier, Coates & Dumanoir 1971 (SPWLA 16th Annual
//     Logging Symposium)，转引 AK04 ch.4。
//   · φ 密度（体积密度→孔隙度）：AK04 ch.3；ρma 文献值：砂岩 2.65 /
//     石灰岩 2.71 / 白云岩 2.87 g/cm³，淡水 ρf 1.0 g/cm³。
//   · φ 中子（氢指数直读，% → v/v 显式换算）：AK04 ch.3。
//   · φ 声波 Wyllie 时间平均：Wyllie, Gregory & Gardner 1956 (Petroleum
//     Transactions, AIME)；砂岩 Δtma 182 µs/m（55.5 µs/ft）、淡水 Δtf
//     620 µs/m（189 µs/ft），AK04 ch.3；Cp 压实校正因子（1 = 不校正）。
//   · Sw Archie：Archie 1942 (Transactions of AIME 146)；a/m/n 文献常用
//     值：a=1/m=2/n=2（碳酸盐岩经典）或 Humble a=0.62/m=2.15（砂岩），
//     AK04 ch.6——全部显式参数，核函数不内置默认。
//   · 深度对齐（跨源线性重采样、不外推、缺失不跨接）：本 ledger 轮1 钉死
//     的策略，非文献公式，语义见 resampleLinear 注释。

#include <vector>

namespace paleo::petrophys
{

// ---- Vsh：GR 基线 → 黏土体积 ---------------------------------------------------
// 四变体共用线性伽马指数 IGR = (GR − grMin)/(grMax − grMin)，先钳 [0,1]
// （AK04 惯例：IGR 即 Vsh 的线性估计，物理值域 [0,1]；非线性变体只对
// [0,1] 内的 IGR 有定义，外推无物理意义）。grMin/grMax 为该井段的纯砂岩/
// 纯泥岩基线（显式参数或井内极值，由调用方定）。
// 前置条件 grMax > grMin（服务层校验；违反时核函数输出全 NaN，不静默）。
// 四变体在 IGR=0 → Vsh=0，IGR=1 → Vsh≈1（Clavier 精确 1.0）。
void vshGrLinear(const double *gr, int n, double grMin, double grMax, double *out);
void vshGrLarionovYoung(const double *gr, int n, double grMin, double grMax, double *out);
void vshGrLarionovOld(const double *gr, int n, double grMin, double grMax, double *out);
void vshGrClavier(const double *gr, int n, double grMin, double grMax, double *out);

// 井内极值基线（grAutoBaseline 语义）：返回非空样本的 [min, max]；全空 →
// false（无基线可谈，调用方报错）。
bool grExtrema(const double *gr, int n, double *minOut, double *maxOut);

// ---- 孔隙度 ---------------------------------------------------------------------
// 密度：φD = (ρma − ρb)/(ρma − ρf)。前置 ρma > ρf（违反 → 全 NaN）。
// 负 φ（气效应/重矿物）如实保留，QC 面标记。
void phiDensity(const double *rhob, int n, double rhoMa, double rhoFluid, double *out);

// 中子：直读氢指数；inPercent=true 时按 % → v/v 除以 100（LAS 中 NPHI 常见
// 两口径并存，显式参数钉死，不猜单位）。
void phiNeutron(const double *nphi, int n, bool inPercent, double *out);

// 声波 Wyllie：φS = (Δt − Δtma)/(Δtf − Δtma) · (1/Cp)。cpFactor = Cp ≥ 1
// （1 = 不校正；Δt 单位与 Δtma/Δtf 一致，µs/m 或 µs/ft 由调用方保证）。
// 前置 (Δtf − Δtma) 与 Cp 同号非零（违反 → 全 NaN）。
void phiSonicWyllie(const double *dt, int n, double dtMa, double dtFluid,
                    double cpFactor, double *out);

// ---- 含水饱和度 -----------------------------------------------------------------
// Archie：Sw = (a·Rw / (φ^m · Rt))^(1/n)。
// 域约定：φ ≤ 0 或 φ > 1 → NaN（孔隙度物理域外，无饱和度意义）；Rt ≤ 0 →
// NaN；a/Rw/m/n ≤ 0 → 全 NaN（公式参数域外）。φ>0 且 Rt>0 时 IEEE 如实
// （可 >1，气洗/轻烃带真实现象，QC 面标记）。
void swArchie(const double *phi, const double *rt, int n,
              double a, double m, double nSat, double rw, double *out);

// ---- 深度对齐（轮1 钉死策略）---------------------------------------------------
// 把 (srcDepths → srcValues) 线性重采样到 refDepths 网格：
//   · 参考点落在 src 深度范围外 → NaN（不外推）；
//   · 插值两端任一 NaN → NaN（缺失不跨接）；
//   · srcDepths 须严格递增且与 srcValues 等长，违反 → 返回 false（out 全 NaN）；
//   · src 为空 → 返回 true，out 全 NaN（整源缺失）。
// 参考点恰在边界上取边界值（t=0/1 端点约定）。
bool resampleLinear(const std::vector<double> &refDepths,
                    const std::vector<double> &srcDepths,
                    const std::vector<double> &srcValues,
                    std::vector<double> &out);

// ---- 曲线 QC --------------------------------------------------------------------
struct CurveStats
{
  int n = 0;               // 总样本数（含 null）
  int valid = 0;           // 非 NaN 样本
  int nulls = 0;           // NaN 样本
  double nullRate = 0.0;   // nulls / n（n=0 时 0）
  double min = 0.0, max = 0.0;
  double mean = 0.0;
  double stddev = 0.0;     // 样本标准差（n−1 分母；valid<2 → NaN）
};

// 摘要统计：valid==0 时 min/max/mean/stddev = NaN（无数据不臆造 0）。
CurveStats curveStats(const double *x, int n);

struct DepthInterval
{
  double from = 0.0, to = 0.0;  // 起止深度（区间内首末样本深度）
  int fromIndex = -1, toIndex = -1;
  int samples = 0;              // 段内样本数
};

// 异常区间：value < lo 或 value > hi 的连续非空样本段。NaN 断段——缺失样本
// 无证据，不得把两侧越界段连通。返回段按深度升序。
std::vector<DepthInterval> anomalyIntervals(const double *depths, const double *x,
                                            int n, double lo, double hi);

} // namespace paleo::petrophys
