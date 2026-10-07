// 层：数据
#pragma once

#include "faultpath.h"
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

// 隔断感知档（方向67，双轨不是替换）：
//   enabled 且 polygons 非空时，样本对的滞后距改用绕障测地距离（faultpath 的
//   barrier Dijkstra，每样本一个距离场）；测地不可达（隔断完全隔开两侧）的对
//   不进实验变差累积——两侧不属于同一连通结构域。方向过滤仍按欧氏位移判。
//   代价 O(n · 格网单元数)：本核限 n ≤ 512（超限 InvalidInput，如实报因）。
//   gridResolution = 测地场沿样本范围最长边的格数（0 = 默认 256；钳到 [32,1024]，
//   总单元数再钳 ≤ 4×10⁶）。
//   enabled = false（默认）或 polygons 为空 → 走既有欧氏距口径，逐位不变。
struct VariogramBarriers
{
  bool enabled = false;
  std::vector<BarrierPolygon> polygons;
  int gridResolution = 0;
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
  bool barrierAware = false;  // true = 本次累积走测地滞后距（血缘 method 细分用）
  int unreachablePairs = 0;   // 隔断感知档下跨隔断（测地不可达）被跳过的对数
};

ExperimentalVariogram experimentalVariogram( const std::vector<Sample> &samples,
    double lag, int nLags, const VariogramDirection &direction = {},
    const VariogramBarriers &barriers = {} );

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
// 几何各向异性：anisotropyRatio ≥ 1 且 azimuthDeg = 长变程方向（走向，从北
// 顺时针）时，垂直方向有效距离 ×ratio → 垂直变程 = range/ratio。
// ratio = 1 时 semivariance(dx,dy) 与各向同性 semivariance(h) 严格一致。
// 垂向比（三维消费方专用，2D 调用方 dz=0 恒不受影响）：verticalRangeRatio ≥ 1
// 时 dz 分量 ×ratio → 垂向变程 = range/ratio（地层格架纵向几米~几十米、横向上
// 百米的典型口径）；< 1 按 1 处理（与水平比同语义钳制）。
struct VariogramModel
{
  VariogramModelType type = VariogramModelType::Spherical;
  double nugget = 0;
  double sill = 0;
  double range = 1;
  double anisotropyRatio = 1;
  double azimuthDeg = 0;
  double verticalRangeRatio = 1;

  // 各向同性口径（诊断/全向/拟合）。
  double semivariance( double h ) const;
  // 几何各向异性口径（克里金/SGS 方程组用：传坐标差）。
  double semivariance( double dx, double dy ) const;
  // 三维口径（stratgrid IJK 格架上的 SGS 用）：dz = 0 精确委托 2D 重载，
  // 2D 消费方逐位不变。
  double semivariance( double dx, double dy, double dz ) const;
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
