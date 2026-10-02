// 层：数据
#pragma once
#include <QString>
#include <functional>
#include <vector>

// gridsolver — 连续曲率张力样条网格化核（纯数值，仅 QtCore 字符串作错误通道）。
// 解 Smith & Wessel (1990) 型方程 (1−T)·∇⁴z − T·γ·∇²z = 0（γ=4/(dx·dy)），
// T=0 为 Briggs (1974) 最小曲率，T→1 趋膜方程（谐和插值）；Gauss-Seidel +
// SOR 迭代。GMT surface 同族算法，离散格式为本文显式推导（见 .cpp 注释）。
//
// 坐标/网格约定：北向上，originX/originY 为左上角像元**边**（pixel-is-area，
// 与 GDAL/BinnedHorizon 同口径）；行沿 −y、列沿 +x。散点落格：
// col = floor((x−originX)/dx)，row = floor((originY−y)/dy)。
//
// NaN 语义：核内部 NaN=缺失。屏障格输出 NaN；散点不覆盖的自由格由方程
// 填充；越界散点拒绝（计数入 stats.rejected，不静默丢弃）。
//
// 数据约束（GMT surface 默认语义）：每点归属最近节点，节点冻结为该节点
// 所属点的均值（Dirichlet），松弛只更新自由节点。亚格偏移按最近节点赋
// 值量化（半像元 × 局部梯度的量级）——与 GMT surface 的默认节点赋值同
// 口径，如实计入精度（解析面测试容差反映该语义）。曾试过 Briggs 式
// 「数据点位残差反传」迭代约束，与松弛算子在屏障邻域形成持续极限环
// （不收敛/发散），已弃用——冻结 Dirichlet 无竞争投影，恒稳。
//
// 进度/取消：核不掺和线程/任务——onSweep 每扫完一遍回调（sweep 单调递增，
// 调用方据此报进度）；cancel 置真则当遍结束即停，结果不可用（返回 false +
// "canceled"）。

#include <cstdint>

namespace paleo::gridsolver
{

struct ScatterPoint
{
  double x = 0, y = 0, z = 0;
};

struct GridGeometry
{
  int cols = 0, rows = 0;
  double originX = 0, originY = 0; // 左上角像元边
  double dx = 0, dy = 0;           // 像元尺寸（>0；dy 为正数，行向 −y）
  bool isValid() const { return cols > 0 && rows > 0 && dx > 0 && dy > 0; }
};

// 网格几何守卫（审计 #33 同口径）：像元总数超 1 亿或维度超限时拒绝
//（返回默认构造的无效几何 + error），防前置整网格分配 OOM。
GridGeometry geometryForExtent(double minX, double maxX, double minY, double maxY,
                               double cellSize, QString *error = nullptr);

struct GriddingParams
{
  double tension = 0.0;      // 张力 T ∈ [0,1)；0=最小曲率，大值抑制过冲
  int maxSweeps = 500;       // 迭代上限
  double convergence = 1e-4; // 收敛阈（相对散点 z 极差）
  double relaxation = 1.5;   // SOR ω ∈ (0,2]；双调和 GS 的实用过松弛
};

// 协作式取消 + 每遍进度。onSweep 可为空。
struct IterationControl
{
  std::function<bool()> cancelRequested;                    // 返回 true 即停
  std::function<void(int sweep, int maxSweeps, double maxDelta)> onSweep;
};

struct GriddingStats
{
  int sweeps = 0;           // 实跑遍数（松弛步 + 约束步各计一）
  bool converged = false;   // maxDelta < convergence·range 提前停
  double finalDelta = 0;    // 末遍最大更新量（z 单位）
  int constrainedNodes = 0; // 散点归属的最近节点数（Dirichlet 冻结）
  int collisions = 0;       // 同一最近节点多点（节点值 = 均值）
  int rejected = 0;         // 越界/非有限散点
  double zMin = 0, zMax = 0; // 散点极值（收敛规格化基准）
};

// 主入口：散点 + 网格 + 屏障掩码（rows*cols，非 0=屏障格；可空）→ 栅格。
// outZ 尺寸 rows*cols（row-major，行 0 = 最大 y）。屏障格与无数据域外格
// 为 NaN。points 为空 → false + error。
bool solveMinimumCurvature(const std::vector<ScatterPoint> &points,
                           const GridGeometry &geometry, const GriddingParams &params,
                           const std::uint8_t *barrierMask, std::vector<float> *outZ,
                           GriddingStats *stats, QString *error = nullptr,
                           const IterationControl &control = {});

// 质量面：每格中心到最近散点的欧氏距离（与 z 同平面单位）。核内部静态
// KD-tree；points 为空 → false + error。屏障格照常给距离（纯几何量）。
std::vector<float> distanceToData(const std::vector<ScatterPoint> &points,
                                  const GridGeometry &geometry, QString *error = nullptr);

// 栅格双线性采样（供 CV 残差评估等）。越界或 2×2 邻域含 NaN → NaN。
float sampleBilinear(const std::vector<float> &z, const GridGeometry &geometry,
                     double x, double y);

struct CrossValidationResult
{
  int folds = 0;            // 实际留出的点数
  double rms = 0, mean = 0, maxAbs = 0; // 残差 = 真值 − 估值（z 单位）
  std::vector<float> residuals;         // 与留出点同序（子采样后）
  std::vector<int> heldOutIndex;        // 原始点索引（子采样后）
};

// 留一法交叉验证：均匀子采样至多 maxPoints 个点，逐点从散点集中剔除、
// 重解方程、在原点位双线性采样估值。fullScans=false 时每折迭代上限减半
//（QC 快档；结果偏保守，文档如实）。points < 2 → false + error。
bool crossValidateLeaveOneOut(const std::vector<ScatterPoint> &points,
                              const GridGeometry &geometry, const GriddingParams &params,
                              int maxPoints, bool fullScans, CrossValidationResult *out,
                              QString *error = nullptr,
                              const IterationControl &control = {});

} // namespace paleo::gridsolver
