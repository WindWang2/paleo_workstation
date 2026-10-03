// 层：数据
#pragma once

// inversion/sparse — 稀疏脉冲反演（FISTA，自研不引库）。
//
// 目标：min_r 0.5·||A·r − s||² + λ·||r||₁，A = 子波褶积算子（same 输出，
// 子波 t=0 样对齐事件位）。解法：FISTA 加速近端梯度（软阈值），Lipschitz
// 常数用幂迭代估计。停机：解相对变化 < tolerance 或达迭代上限；λ 缺省
// 0.05·max|Aᵀs|（保守稀疏度，事件漏检弱于过阈值噪声）。
//
// 振幅标定（#141）：s = trace / amplitudeScale 后再求反射系数；钳位比例超过
// maxClampedFraction 判失败（不再按 (1+r)/(1−r) 静默发散）。
// 阻抗由稀疏反射系数递推 Z(i+1)=Z(i)(1+r)/(1−r)，种子取低频模型首样
//（无低频 → 相对阻抗，种子 1.0）。残差能量比 ||Ar−s||²/||s||² 入 QC。

#include "wavelet.h"

#include <string>
#include <vector>

namespace paleo::inversion
{

struct SparseSpikeOptions
{
  double lambda = 0.0;               // L1 权重；<=0 → 自动 0.05·max|Aᵀs|
  int maxIterations = 200;           // 迭代上限 [8, 10000]
  double relativeTolerance = 1e-4;   // 解相对变化停机
  // 道振幅 / 单位反射系数（#141）。<=0 → 用子波 amplitudeScale；仍无 → 1（未标定）。
  // 显式 lambda 按标定后的反射系数单位解释。
  double amplitudeScale = 0.0;
  // 非零反射系数中触及 ±kMaxReflectivity 钳位的比例上限；超过判失败。
  double maxClampedFraction = 0.05;
};

struct SparseSpikeResult
{
  bool ok = false;
  std::string reason;
  std::vector<float> reflectivity;     // 稀疏反射系数（长度 n）
  std::vector<float> impedance;        // 递推阻抗（低频种子；无低频 = 相对）
  double residualEnergyRatio = 0.0;    // ||Ar−s||²/||s||²
  int iterations = 0;
  bool converged = false;
  double lambdaUsed = 0.0;
  double amplitudeScaleUsed = 1.0;   // 实际使用的振幅标定
  bool amplitudeCalibrated = false;  // false = 未标定（道振幅直接当反射系数）
  double clampedFraction = 0.0;      // 非零反射系数中触及钳位的比例
};

// trace/lowFreq 长度均为 n；lowFreq 可空（相对阻抗口径）。
SparseSpikeResult sparseSpikeInversion(const float *trace, int n, double sampleIntervalMs,
                                       const Wavelet &wavelet, const float *lowFreq,
                                       const SparseSpikeOptions &options = {});

} // namespace paleo::inversion
