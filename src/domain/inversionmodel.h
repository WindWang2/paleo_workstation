// 层：数据
#pragma once

// Shared value models; numerical kernels live in algorithms/inversion.
#include <string>
#include <vector>

namespace paleo::inversion {
struct Wavelet
{
  double sampleIntervalMs = 2.0;
  double t0Ms = 0.0;
  std::vector<float> samples;
  // 振幅标定（#141）：地震道振幅 ≈ 反射系数 ⊛ (amplitudeScale · samples)。
  // 井旁道提取时由最小二乘解的峰值给出（归一化前的尺度）；0 = 未标定
  //（解析子波 / 旧资产），反演按 1 处理并在结果中标 amplitudeCalibrated=false。
  double amplitudeScale = 0.0;

  int sampleCount() const { return int(samples.size()); }
  bool isEmpty() const { return samples.empty(); }
  double lengthMs() const;       // (n-1)·dt；空 → 0
  double peakAmplitude() const;  // max|x|；空 → 0
  double dominantFreqHz() const; // 振幅谱峰值频率（FFT 零填充；空 → 0）
};

struct ReflSpike
{
  double twtMs = 0.0;
  float amplitude = 0.0f;
};

// 井阻抗曲线（TWT 域，nS 样与体网格一致，NaN = 缺失段）。
struct LowFreqWellTrace
{
  double x = 0.0;
  double y = 0.0;
  std::vector<float> impedance;
  std::vector<double> xSamples, ySamples; // optional, same nS; NaN means no observed position
};

// 层位 TWT 网格：nIl*nXl 行主序（行 = inline），NaN = 缺失格。
struct HorizonTwtGrid
{
  std::string name;
  std::vector<float> twtMs;
};

struct LowFreqModelInput
{
  // 体时间网格
  double t0Ms = 0.0;
  double dtMs = 2.0;
  int nIl = 0;
  int nXl = 0;
  int nS = 0;

  // 测网仿射几何：格 (i,j) 中心 = origin + i·ilStep + j·xlStep（XY 增量向量）。
  double originX = 0.0;
  double originY = 0.0;
  double ilStepX = 0.0;
  double ilStepY = 0.0;
  double xlStepX = 0.0;
  double xlStepY = 0.0;

  std::vector<LowFreqWellTrace> wells;
  std::vector<HorizonTwtGrid> horizons; // 任意序，内部按平均 TWT 升序

  double lowCutHz = 8.0; // 模型频段上界；平滑窗 = 1000/lowCut ms
  double idwPower = 2.0; // IDW 权重 1/d^power
};

struct LowFreqModelResult
{
  bool ok = false;
  std::string reason;

  double t0Ms = 0.0;
  double dtMs = 2.0;
  int nIl = 0;
  int nXl = 0;
  int nS = 0;
  double originX = 0.0;
  double originY = 0.0;
  double ilStepX = 0.0;
  double ilStepY = 0.0;
  double xlStepX = 0.0;
  double xlStepY = 0.0;
  double idwPower = 2.0;

  double lowCutHz = 0.0;        // 频段上界（产物 extra 必带）
  double smoothingWindowMs = 0.0; // 实际平滑窗 = nWin·dt
  int layersUsed = 0;           // 有效层数；0 = 全局趋势回退

  // 层模式：每格每层阻抗，((il·nXl)+xl)·nL + l；NaN = 该格该层无井控。
  std::vector<float> layerImpedance;
  // 层模式：升序层位边界（拷贝自输入，去掉全缺层位）。
  std::vector<HorizonTwtGrid> boundaries;
  // 层模式：各边界平均 TWT（与 boundaries 同序，预计算免逐道重扫）。
  std::vector<double> boundaryMeanTwt;
  // 回退模式：低通后的井曲线（结构与输入 wells 一致）。
  std::vector<LowFreqWellTrace> smoothedWells;

  int layerCount() const
  {
    return layersUsed > 0 ? layersUsed : 0;
  }
};

}
