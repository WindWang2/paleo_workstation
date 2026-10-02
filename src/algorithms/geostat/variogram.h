// 层：数据
#pragma once

#include "types.h"

#include <string>
#include <vector>

// geostat/variogram — 实验变差函数与理论模型拟合。
// 半方差 γ(h) = 0.5 · mean[(z(sᵢ) − z(sⱼ))²]，对全样本对 O(n²) 累积。
// 方向变差函数按「方位角±容差」过滤对，h 与 -h 计同一对（对称性）。
// 层：数据
namespace paleo::geostat
{

// azimuthDeg 从北顺时针（x 东 y 北）。omnidirectional = true 时不过滤。
struct VariogramDirection
{
  bool omnidirectional = true;
  double azimuthDeg = 0;
  double toleranceDeg = 22.5;
};

struct ExperimentalVariogram
{
  Status status = Status::InvalidInput;
  std::string message;
  double lag = 0; // 输入原样保存
  int nLags = 0;
  std::vector<double> lagDistance;  // 每 lag 的平均对距（pairCount=0 的 lag 为 0）
  std::vector<double> semivariance; // 每 lag 的半方差（pairCount=0 的 lag 为 0）
  std::vector<int> pairCount;
};

ExperimentalVariogram experimentalVariogram( const std::vector<Sample> &samples,
    double lag, int nLags, const VariogramDirection &direction = {} );

enum class VariogramModelType
{
  Spherical,
  Exponential,
  Gaussian
};

// γ(h) = nugget + sill · shape(h / range)，shape ∈ [0, 1)。
// h = 0 时 γ = 0（克里金矩阵对角元口径——离散点重合仍 0，块金是 h→0⁺ 跳变）。
// sill 是拱高（partial sill），总基台 = nugget + sill。
// range 口径 = 实用变程：球状模型 h ≥ range 时达总基台；指数/高斯取 ~95%
// 基台距离（指数 shape = 1 − exp(−3h/range)，高斯 shape = 1 − exp(−3h²/range²)）。
struct VariogramModel
{
  VariogramModelType type = VariogramModelType::Spherical;
  double nugget = 0;
  double sill = 0;
  double range = 1;

  double semivariance( double h ) const;
};

struct VariogramFit
{
  Status status = Status::InvalidInput;
  std::string message;
  VariogramModel model;
  double rmse = 0; // 对实验点的均方根残差
  double r2 = 0;   // 1 − SSE/SST；可为负（劣于均值），如实报告
  int usedLags = 0;
};

// 最小二乘拟合：固定 range 时 (nugget, sill) 线性（2×2 正规方程，非负钳制），
// range 在 [hMin/4, hMax·4] 对数扫描 ~121 档 + 每档局部细化，取 SSE 最小。
VariogramFit fitVariogram( const ExperimentalVariogram &experimental,
                           VariogramModelType type );

} // namespace paleo::geostat
