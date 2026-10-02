// 层：数据
#include "gridsolver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>

// ---------------------------------------------------------------------------
// 离散格式（显式推导，审查口径）
//
// 方程（Smith & Wessel 1990 连续曲率样条；T=0 退化为 Briggs 1974 最小曲率）：
//     (1−T)·∇⁴z − T·γ·∇²z = 0，  γ = 4/(dx·dy)。
// γ 的量纲是 1/长度²，系数 4 使 T=0.5 时两算子在半波长尺度上大致平衡
//（与 GMT surface 的张力标度同族；具体常数不影响 T=0 的精确性与单调性
// 结论，只移动中间张力的手感）。
//
// 记 u = 1/dx²、v = 1/dy²（i=行/y、j=列/x），标准差分算子：
//   δ²x z = (z_{i,j+1} − 2z_{i,j} + z_{i,j−1})·u
//   δ²y z = (z_{i+1,j} − 2z_{i,j} + z_{i−1,j})·v
//   ∇⁴z  = δ²xδ²x z + δ²yδ²y z + 2·δ²xδ²y z
// 逐项展开后，以 z_{i,j} 为未知元的线性方程邻居系数为：
//   c2x = (1−T)·u²          （(i, j±2)）
//   c1x = −(1−T)(4u²+4uv) − T·γ·u   （(i, j±1)）
//   c2y = (1−T)·v²          （(i±2, j)）
//   c1y = −(1−T)(4v²+4uv) − T·γ·v   （(i±1, j)）
//   cd  = 2(1−T)·uv         （四角 (i±1, j±1)）
//   a0  = (1−T)(6u²+6v²+8uv) + 2T·γ·(u+v)   （对角，恒正）
// Gauss-Seidel：z_{i,j} ← −Σ c_n·z_n / a0，再 SOR 外推
// z ← z + ω(z_gs − z)。
//
// 边界：越界邻居取**线性外推幽灵格**（沿边一阶外推：z(−k) = z₀ − k(z₁−z₀)）。
// 线性函数被精确保持（外推即真值），故平面在边界处仍是离散方程的精确解；
// 曲面边界为 O(h²) 截断的自然类边界。
// 屏障：邻居为屏障格时以中心值代入（镜像/无通量内边界）——断层两盘
// 各自松弛，互不插值。
//
// 精确性基准（测试口径）：线性面使全部算子恒为零、二次面使 ∇⁴ 恒为零，
// 故 T=0 时二者是离散方程的精确解；正弦面误差由采样密度控制。
// ---------------------------------------------------------------------------

namespace paleo::gridsolver
{
namespace
{

constexpr qint64 kMaxCellCount = 100'000'000; // 与 horizonbinner/PaleoAlgoGuards 同口径
constexpr int kMinDim = 4;                    // i±2 模板需要的最小网格
constexpr int kIdwNeighbors = 8;              // 初始化 IDW 的近邻数

// ---- 静态 2D KD-tree（中位数分裂；仅服务本核的近邻查询）--------------------

struct KdTree
{
  struct Node
  {
    double x, y;
    int index; // 原始散点索引
  };
  std::vector<Node> nodes; // 中序即树结构：根在 0，按 axis 交替分裂

  explicit KdTree(const std::vector<ScatterPoint> &pts)
  {
    nodes.reserve(pts.size());
    for (int i = 0; i < static_cast<int>(pts.size()); ++i)
      nodes.push_back({pts[i].x, pts[i].y, i});
    build(0, static_cast<int>(nodes.size()), 0);
  }

  void build(int lo, int hi, int depth)
  {
    if (hi - lo <= 1)
      return;
    const int mid = (lo + hi) / 2;
    const bool byX = depth % 2 == 0;
    std::nth_element(nodes.begin() + lo, nodes.begin() + mid, nodes.begin() + hi,
                     [byX](const Node &a, const Node &b)
                     { return byX ? a.x < b.x : a.y < b.y; });
    build(lo, mid, depth + 1);
    build(mid + 1, hi, depth + 1);
  }

  // k 近邻（距离² 升序）。maxHeap 语义：队首是当前 k 个中最差者。
  void knn(double qx, double qy, int k, std::vector<std::pair<double, int>> &out) const
  {
    out.clear();
    if (nodes.empty())
      return;
    using Entry = std::pair<double, int>; // (−dist², index)：priority_queue 取最大
    std::priority_queue<Entry> heap;
    visit(0, static_cast<int>(nodes.size()), 0, qx, qy, k, heap);
    out.resize(heap.size());
    for (std::size_t s = out.size(); s > 0; --s)
    {
      out[s - 1] = {-heap.top().first, heap.top().second};
      heap.pop();
    }
  }

  void visit(int lo, int hi, int depth, double qx, double qy, int k,
             std::priority_queue<std::pair<double, int>> &heap) const
  {
    if (lo >= hi)
      return;
    const int mid = (lo + hi) / 2;
    const Node &n = nodes[mid];
    const double dxq = qx - n.x, dyq = qy - n.y;
    const double d2 = dxq * dxq + dyq * dyq;
    const int idx = n.index;
    if (static_cast<int>(heap.size()) < k)
      heap.push({-d2, idx});
    else if (d2 < -heap.top().first)
    {
      heap.pop();
      heap.push({-d2, idx});
    }
    const bool byX = depth % 2 == 0;
    const double delta = byX ? dxq : dyq;
    const int nearLo = lo, nearHi = mid, farLo = mid + 1, farHi = hi;
    if (delta < 0)
      visit(nearLo, nearHi, depth + 1, qx, qy, k, heap);
    else
      visit(farLo, farHi, depth + 1, qx, qy, k, heap);
    const double worst = heap.empty() ? std::numeric_limits<double>::infinity()
                                      : -heap.top().first;
    if (static_cast<int>(heap.size()) < k || delta * delta < worst)
    {
      if (delta < 0)
        visit(farLo, farHi, depth + 1, qx, qy, k, heap);
      else
        visit(nearLo, nearHi, depth + 1, qx, qy, k, heap);
    }
  }
};

// 北向上散点 → 像元下标。越界返回 false。
inline bool cellOf(const GridGeometry &g, double x, double y, int *row, int *col)
{
  const double cf = (x - g.originX) / g.dx;
  const double rf = (g.originY - y) / g.dy;
  if (!(cf >= 0.0) || !(rf >= 0.0))
    return false;
  const int c = static_cast<int>(cf);
  const int r = static_cast<int>(rf);
  if (c >= g.cols || r >= g.rows)
    return false;
  *row = r;
  *col = c;
  return true;
}

inline double clampd(double v, double lo, double hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

// ---------------------------------------------------------------------------
// 守卫
// ---------------------------------------------------------------------------

GridGeometry geometryForExtent(double minX, double maxX, double minY, double maxY,
                               double cellSize, QString *error)
{
  const auto fail = [error](const char *msg)
  {
    if (error)
      *error = QString::fromUtf8(msg);
    return GridGeometry();
  };
  if (!(std::isfinite(minX) && std::isfinite(maxX) && std::isfinite(minY) &&
        std::isfinite(maxY) && std::isfinite(cellSize)))
    return fail("grid extent or cell size is not finite");
  if (maxX <= minX || maxY <= minY)
    return fail("grid extent is empty (min must be strictly less than max)");
  if (!(cellSize > 0.0))
    return fail("cell size must be positive");
  constexpr double kMaxDim = 2147483647.0;
  const double colsD = std::ceil((maxX - minX) / cellSize);
  const double rowsD = std::ceil((maxY - minY) / cellSize);
  if (colsD > kMaxDim || rowsD > kMaxDim)
    return fail("requested grid dimensions exceed int limits — increase the cell size");
  const qint64 cells = static_cast<qint64>(colsD) * static_cast<qint64>(rowsD);
  if (cells > kMaxCellCount)
  {
    if (error)
      *error = QStringLiteral("requested grid %1x%2 (%3 cells) exceeds the %4-cell "
                              "budget — increase the cell size or reduce the extent")
                   .arg(static_cast<qint64>(colsD))
                   .arg(static_cast<qint64>(rowsD))
                   .arg(cells)
                   .arg(kMaxCellCount);
    return GridGeometry();
  }
  GridGeometry g;
  g.cols = static_cast<int>(colsD);
  g.rows = static_cast<int>(rowsD);
  g.originX = minX;
  g.originY = maxY; // 北向上：左上角
  g.dx = cellSize;
  g.dy = cellSize;
  return g;
}

// ---------------------------------------------------------------------------
// 主求解
// ---------------------------------------------------------------------------

// 多级级联的细级初值要用上级解上采样（定义在文件后段）。
float sampleBilinear(const std::vector<float> &z, const GridGeometry &geometry,
                     double x, double y);

bool solveMinimumCurvature(const std::vector<ScatterPoint> &points,
                           const GridGeometry &geometry, const GriddingParams &params,
                           const std::uint8_t *barrierMask, std::vector<float> *outZ,
                           GriddingStats *stats, QString *error,
                           const IterationControl &control)
{
  const auto fail = [error](const QString &msg)
  {
    if (error)
      *error = msg;
    return false;
  };
  if (!geometry.isValid())
    return fail(QStringLiteral("grid geometry is invalid"));
  if (geometry.cols < kMinDim || geometry.rows < kMinDim)
    return fail(QStringLiteral("grid must be at least %1x%1 cells for the curvature "
                               "stencil")
                    .arg(kMinDim));
  if (points.empty())
    return fail(QStringLiteral("no scatter points to grid"));
  if (!(params.tension >= 0.0 && params.tension < 1.0))
    return fail(QStringLiteral("tension must be in [0, 1)"));
  if (!(params.relaxation > 0.0 && params.relaxation <= 2.0))
    return fail(QStringLiteral("SOR relaxation factor must be in (0, 2]"));
  if (params.maxSweeps < 1)
    return fail(QStringLiteral("maxSweeps must be >= 1"));
  if (!(params.convergence > 0.0))
    return fail(QStringLiteral("convergence threshold must be positive"));
  if (!outZ)
    return fail(QStringLiteral("output raster pointer is null"));

  const int cols = geometry.cols, rows = geometry.rows;
  const std::size_t n = static_cast<std::size_t>(rows) * cols;

  // ---- 散点 → 最近节点约束（CSR 分组）+ 极差 -------------------------------
  // 约束语义（GMT surface 默认）：节点冻结为所属点均值（Dirichlet），松弛
  // 只更新自由节点——无竞争投影，恒稳；亚格偏移按最近节点量化（见 .h）。
  double zMin = std::numeric_limits<double>::infinity();
  double zMax = -std::numeric_limits<double>::infinity();
  GriddingStats st;
  std::vector<ScatterPoint> finitePoints;
  finitePoints.reserve(points.size());
  std::vector<std::pair<std::size_t, int>> nodePointPairs; // (nodeIdx, 点序号)
  nodePointPairs.reserve(points.size());
  const auto nodeIndexOf = [&](const ScatterPoint &p, int *row, int *col)
  {
    const double rf = (geometry.originY - p.y) / geometry.dy - 0.5;
    const double cf = (p.x - geometry.originX) / geometry.dx - 0.5;
    *row = static_cast<int>(std::lround(rf));
    *col = static_cast<int>(std::lround(cf));
    *row = *row < 0 ? 0 : (*row >= rows ? rows - 1 : *row);
    *col = *col < 0 ? 0 : (*col >= cols ? cols - 1 : *col);
  };
  for (const ScatterPoint &p : points)
  {
    if (!(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)))
    {
      ++st.rejected;
      continue;
    }
    int r = 0, c = 0;
    if (!cellOf(geometry, p.x, p.y, &r, &c))
    {
      ++st.rejected;
      continue;
    }
    nodeIndexOf(p, &r, &c); // 覆写为最近节点
    finitePoints.push_back(p);
    nodePointPairs.push_back({static_cast<std::size_t>(r) * cols + c,
                              static_cast<int>(finitePoints.size()) - 1});
    zMin = std::min(zMin, p.z);
    zMax = std::max(zMax, p.z);
  }
  if (finitePoints.empty())
    return fail(QStringLiteral("all scatter points were rejected (outside the grid or "
                               "non-finite)"));
  std::sort(nodePointPairs.begin(), nodePointPairs.end());
  for (std::size_t k = 0; k < nodePointPairs.size();)
  {
    std::size_t end = k;
    while (end < nodePointPairs.size() &&
           nodePointPairs[end].first == nodePointPairs[k].first)
      ++end;
    ++st.constrainedNodes;
    if (end - k > 1)
      ++st.collisions;
    k = end;
  }
  st.zMin = zMin;
  st.zMax = zMax;
  const double zRange = std::max(zMax - zMin, 1e-12);

  // ---- 多级粗→细级联（多网格思想，GMT surface 的预处理同思路）-----------
  // 双调和 GS 的低频误差衰减极慢（细级单跑 tol=1e-6 需 ~2.4 万遍）；粗级
  // 网格上同尺度误差是高频、几十遍即消。逐级 2 倍加密、上级解双线性上采
  // 样作下级初值，末级只修高频残差。
  std::vector<GridGeometry> levels;
  levels.push_back(geometry);
  while (levels.back().rows >= 2 * kMinDim && levels.back().cols >= 2 * kMinDim)
  {
    GridGeometry coarse = levels.back();
    coarse.rows = (coarse.rows + 1) / 2;
    coarse.cols = (coarse.cols + 1) / 2;
    coarse.dx *= 2.0;
    coarse.dy *= 2.0;
    levels.push_back(coarse);
  }
  std::reverse(levels.begin(), levels.end()); // 最粗在前

  // 屏障掩码逐级预粗化：mask[li] 由 mask[li+1]（更细一级）2×2 足迹收缩
  // 而来——不能从最细级一步 /2 折算（中间级缓冲更小，直接越界写堆）。
  std::vector<std::vector<std::uint8_t>> levelMasks(levels.size());
  if (barrierMask)
  {
    levelMasks.back().assign(barrierMask, barrierMask + n);
    for (std::size_t li = levels.size() - 1; li-- > 0;)
    {
      const GridGeometry &fine = levels[li + 1];
      const GridGeometry &coarse = levels[li];
      levelMasks[li].assign(static_cast<std::size_t>(coarse.rows) * coarse.cols, 0);
      for (int i = 0; i < fine.rows; ++i)
        for (int j = 0; j < fine.cols; ++j)
          if (levelMasks[li + 1][static_cast<std::size_t>(i) * fine.cols + j])
            levelMasks[li][static_cast<std::size_t>(i / 2) * coarse.cols + j / 2] = 1;
    }
  }

  KdTree tree(finitePoints);
  std::vector<float> z;              // 当前级解
  std::vector<float> zCoarse;        // 上一（更粗）级解
  int progressTotal = params.maxSweeps * static_cast<int>(levels.size());
  int progressDone = 0;
  for (std::size_t li = 0; li < levels.size(); ++li)
  {
    const GridGeometry &g = levels[li];
    const bool finest = li + 1 == levels.size();
    const int lcols = g.cols, lrows = g.rows;
    const std::size_t ln = static_cast<std::size_t>(lrows) * lcols;

    // 本级屏障掩码（已按级预粗化；空 = 无屏障级）。
    const std::uint8_t *mask =
        barrierMask && !levelMasks[li].empty() ? levelMasks[li].data() : nullptr;

    // 本级最近节点约束（CSR）。
    std::vector<std::pair<std::size_t, int>> pairs;
    pairs.reserve(finitePoints.size());
    for (int pi = 0; pi < static_cast<int>(finitePoints.size()); ++pi)
    {
      const ScatterPoint &p = finitePoints[pi];
      // 散点已验在细级网格内；粗级覆盖同域必在内，直接取最近节点。
      const double rf = (g.originY - p.y) / g.dy - 0.5;
      const double cf = (p.x - g.originX) / g.dx - 0.5;
      int r = static_cast<int>(std::lround(rf));
      int c = static_cast<int>(std::lround(cf));
      r = r < 0 ? 0 : (r >= lrows ? lrows - 1 : r);
      c = c < 0 ? 0 : (c >= lcols ? lcols - 1 : c);
      pairs.push_back({static_cast<std::size_t>(r) * lcols + c, pi});
    }
    std::sort(pairs.begin(), pairs.end());

    // ---- 初始化：粗级 IDW；细级 = 上级解上采样（NaN 落点回退 IDW）-------
    z.assign(ln, std::numeric_limits<float>::quiet_NaN());
    std::vector<std::uint8_t> fixed(ln, 0);
    if (mask)
      for (std::size_t k = 0; k < ln; ++k)
        if (mask[k])
          fixed[k] = 1;
    for (std::size_t k = 0; k < pairs.size();)
    {
      std::size_t end = k;
      double sum = 0;
      while (end < pairs.size() && pairs[end].first == pairs[k].first)
      {
        sum += finitePoints[pairs[end].second].z;
        ++end;
      }
      const std::size_t node = pairs[k].first;
      if (!fixed[node]) // 屏障格上的约束点：跳过（屏障优先，数据不复活屏障）
        z[node] = static_cast<float>(sum / static_cast<double>(end - k));
      fixed[node] = 1;
      k = end;
    }
    std::vector<std::pair<double, int>> nbrs;
    for (int i = 0; i < lrows; ++i)
    {
      const double cy = g.originY - (i + 0.5) * g.dy; // 行中心（北向上 −y）
      for (int j = 0; j < lcols; ++j)
      {
        const std::size_t idx = static_cast<std::size_t>(i) * lcols + j;
        if (fixed[idx])
          continue;
        const double cx = g.originX + (j + 0.5) * g.dx;
        double v = std::numeric_limits<double>::quiet_NaN();
        if (li > 0)
          v = sampleBilinear(zCoarse, levels[li - 1], cx, cy); // 上级上采样
        if (std::isnan(v)) // 边缘/屏障邻域回退 IDW（k 近邻 p=2）
        {
          tree.knn(cx, cy,
                   std::min<int>(kIdwNeighbors, static_cast<int>(finitePoints.size())),
                   nbrs);
          double wSum = 0, vSum = 0;
          for (const auto &e : nbrs)
          {
            if (!(e.first > 0.0)) // 与格中心重合（理论不可达，防御）
            {
              wSum = 1.0;
              vSum = finitePoints[e.second].z;
              break;
            }
            const double w = 1.0 / e.first; // e.first = dist²，IDW p=2
            wSum += w;
            vSum += w * finitePoints[e.second].z;
          }
          v = vSum / wSum;
        }
        z[idx] = static_cast<float>(v);
      }
    }

    // ---- 模板系数（本级 dx/dy）---------------------------------------------
    const double T = params.tension;
    const double u = 1.0 / (g.dx * g.dx);
    const double v = 1.0 / (g.dy * g.dy);
    const double gamma = 4.0 / (g.dx * g.dy);
    const double c2x = (1.0 - T) * u * u;
    const double c1x = -(1.0 - T) * (4.0 * u * u + 4.0 * u * v) - T * gamma * u;
    const double c2y = (1.0 - T) * v * v;
    const double c1y = -(1.0 - T) * (4.0 * v * v + 4.0 * u * v) - T * gamma * v;
    const double cd = 2.0 * (1.0 - T) * u * v;
    const double a0 = (1.0 - T) * (6.0 * u * u + 6.0 * v * v + 8.0 * u * v) +
                      2.0 * T * gamma * (u + v);

    // 邻居项求值：越界 → 线性外推幽灵格（保线性精确）；触及屏障 → 该项
    // **丢弃**（不镜像）。行的更新量按保留项重归一：gs = Σ cₙ·vₙ / Σ cₙ
    //（同号负系数 → 正权重凸组合，恒有界，构造性稳定）。无屏障内点
    // Σcₙ = −a0，精确退化为原模板 −Σcₙvₙ/a0——精度口径不变。曾用「屏障
    // 邻居取中心值」镜像（显式或折对角隐式），跨墙 ±2 项与镜像项形成正
    // 反馈，GS/Jacobi 均发散（见 ledger Round 4 诊断），弃用。
    float *zp = z.data();
    const auto isBarrier = [&](int r, int c) -> bool
    {
      return mask && mask[static_cast<std::size_t>(r) * lcols + c];
    };
    // 返回 false = 丢弃（屏障或幽灵锚点为屏障）。
    const auto read = [&](int r, int c, double *out) -> bool
    {
      if (isBarrier(r, c))
        return false;
      *out = zp[static_cast<std::size_t>(r) * lcols + c];
      return true;
    };
    const auto colVal = [&](int r, int c, double *out) -> bool
    {
      if (c >= 0 && c < lcols)
        return read(r, c, out);
      const int c0 = c < 0 ? 0 : lcols - 1;
      const int c1 = c < 0 ? 1 : lcols - 2;
      double a = 0, b = 0;
      if (!read(r, c0, &a) || !read(r, c1, &b))
        return false;
      const double k = c < 0 ? -c : c - (lcols - 1);
      *out = a + (a - b) * k;
      return true;
    };
    const auto val = [&](int r, int c, double *out) -> bool
    {
      if (r >= 0 && r < lrows)
        return colVal(r, c, out);
      const int r0 = r < 0 ? 0 : lrows - 1;
      const int r1 = r < 0 ? 1 : lrows - 2;
      double a = 0, b = 0;
      if (!colVal(r0, c, &a) || !colVal(r1, c, &b))
        return false;
      const double k = r < 0 ? -r : r - (lrows - 1);
      *out = a + (a - b) * k;
      return true;
    };

    const double omega = params.relaxation;
    const double tol = params.convergence * zRange;
    for (int sweep = 1; sweep <= params.maxSweeps; ++sweep)
    {
      double maxDelta = 0.0;
      for (int i = 0; i < lrows; ++i)
      {
        for (int j = 0; j < lcols; ++j)
        {
          const std::size_t idx = static_cast<std::size_t>(i) * lcols + j;
          if (fixed[idx])
            continue;
          const float center = zp[idx];
          double sum = 0.0, coefSum = 0.0;
          const auto add = [&](double coeff, int rr, int cc, bool drop)
          {
            if (drop)
              return; // 跨屏障泄漏项（如 j±1 是墙时的 j±2）
            double t = 0;
            if (!val(rr, cc, &t))
              return; // 屏障/幽灵锚点为屏障 → 丢弃
            sum += coeff * t;
            coefSum += coeff;
          };
          add(c2x, i, j + 2, j + 1 < lcols && isBarrier(i, j + 1));
          add(c2x, i, j - 2, j - 1 >= 0 && isBarrier(i, j - 1));
          add(c1x, i, j + 1, false);
          add(c1x, i, j - 1, false);
          add(c2y, i + 2, j, i + 1 < lrows && isBarrier(i + 1, j));
          add(c2y, i - 2, j, i - 1 >= 0 && isBarrier(i - 1, j));
          add(c1y, i + 1, j, false);
          add(c1y, i - 1, j, false);
          add(cd, i + 1, j + 1, false);
          add(cd, i + 1, j - 1, false);
          add(cd, i - 1, j + 1, false);
          add(cd, i - 1, j - 1, false);
          // 重归一加权平均；全项被丢弃（厚墙包围）→ 保持原值。
          const double gs = coefSum != 0.0 ? sum / coefSum : center;
          const double zn = center + omega * (gs - center);
          const double d = std::fabs(zn - center);
          if (d > maxDelta)
            maxDelta = d;
          zp[idx] = static_cast<float>(zn);
        }
      }
      st.sweeps = progressDone + sweep;
      st.finalDelta = maxDelta;
      if (!std::isfinite(maxDelta))
        return fail(QStringLiteral(
            "iteration diverged (non-finite update) — lower the relaxation factor or "
            "raise tension"));
      if (maxDelta < tol)
      {
        st.converged = true;
        break;
      }
      if (control.onSweep)
        control.onSweep(progressDone + sweep, progressTotal, maxDelta);
      if (control.cancelRequested && control.cancelRequested())
        return fail(QStringLiteral("canceled"));
    }
    progressDone += params.maxSweeps;
    if (finest)
    {
      // 屏障格显式置 NaN（初始化即 NaN，此处双保险）。
      if (barrierMask)
        for (std::size_t k = 0; k < n; ++k)
          if (barrierMask[k])
            z[k] = std::numeric_limits<float>::quiet_NaN();
      *outZ = std::move(z);
    }
    else
    {
      st.converged = false; // 中间级不算收敛（末级说了算）
      zCoarse = std::move(z);
    }
  }
  if (stats)
    *stats = st;
  return true;
}

// ---------------------------------------------------------------------------
// 距数据距离质量面
// ---------------------------------------------------------------------------

std::vector<float> distanceToData(const std::vector<ScatterPoint> &points,
                                  const GridGeometry &geometry, QString *error)
{
  if (!geometry.isValid())
  {
    if (error)
      *error = QStringLiteral("grid geometry is invalid");
    return {};
  }
  if (points.empty())
  {
    if (error)
      *error = QStringLiteral("no scatter points");
    return {};
  }
  KdTree tree(points);
  const std::size_t n = static_cast<std::size_t>(geometry.rows) * geometry.cols;
  std::vector<float> out(n, std::numeric_limits<float>::quiet_NaN());
  std::vector<std::pair<double, int>> best;
  for (int i = 0; i < geometry.rows; ++i)
  {
    const double cy = geometry.originY - (i + 0.5) * geometry.dy;
    for (int j = 0; j < geometry.cols; ++j)
    {
      const double cx = geometry.originX + (j + 0.5) * geometry.dx;
      tree.knn(cx, cy, 1, best);
      if (!best.empty())
        out[static_cast<std::size_t>(i) * geometry.cols + j] =
            static_cast<float>(std::sqrt(best.front().first));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// 双线性采样 + 留一法交叉验证
// ---------------------------------------------------------------------------

float sampleBilinear(const std::vector<float> &z, const GridGeometry &geometry,
                     double x, double y)
{
  const double jf = (x - geometry.originX) / geometry.dx - 0.5;
  const double rf = (geometry.originY - y) / geometry.dy - 0.5;
  if (jf < -0.5 || rf < -0.5 || jf > geometry.cols - 0.5 || rf > geometry.rows - 0.5)
    return std::numeric_limits<float>::quiet_NaN();
  // 网格内采样：窗原点钳进 [0, dim−2]，权重随之钳入 [0,1]（贴边点落在
  // 边缘窗的外推半格上，权重饱和到边界）。
  int j0 = std::clamp(static_cast<int>(std::floor(jf)), 0, geometry.cols - 2);
  int i0 = std::clamp(static_cast<int>(std::floor(rf)), 0, geometry.rows - 2);
  const double fx = clampd(jf - j0, 0.0, 1.0);
  const double fy = clampd(rf - i0, 0.0, 1.0);
  const auto at = [&](int r, int c) -> double
  { return z[static_cast<std::size_t>(r) * geometry.cols + c]; };
  const double z00 = at(i0, j0), z10 = at(i0 + 1, j0), z01 = at(i0, j0 + 1),
               z11 = at(i0 + 1, j0 + 1);
  if (std::isnan(z00) || std::isnan(z10) || std::isnan(z01) || std::isnan(z11))
    return std::numeric_limits<float>::quiet_NaN();
  const double top = z00 * (1.0 - fx) + z01 * fx;
  const double bot = z10 * (1.0 - fx) + z11 * fx;
  return static_cast<float>(top * (1.0 - fy) + bot * fy);
}

bool crossValidateLeaveOneOut(const std::vector<ScatterPoint> &points,
                              const GridGeometry &geometry, const GriddingParams &params,
                              int maxPoints, bool fullScans, CrossValidationResult *out,
                              QString *error, const IterationControl &control)
{
  const auto fail = [error](const QString &msg)
  {
    if (error)
      *error = msg;
    return false;
  };
  if (!out)
    return fail(QStringLiteral("result pointer is null"));
  if (static_cast<int>(points.size()) < 2)
    return fail(QStringLiteral("cross validation needs at least 2 points"));
  if (maxPoints < 1)
    maxPoints = 1;
  maxPoints = std::min<int>(maxPoints, static_cast<int>(points.size()));

  // 均匀子采样（确定性）：折数受控，全量 LOO 的 O(n·solve) 代价不进默认路径。
  std::vector<int> foldIdx;
  foldIdx.reserve(maxPoints);
  if (maxPoints == 1)
    foldIdx.push_back(0);
  else
    for (int k = 0; k < maxPoints; ++k)
      foldIdx.push_back(static_cast<int>(std::llround(
          static_cast<double>(k) * static_cast<int>(points.size() - 1) / (maxPoints - 1))));

  CrossValidationResult res;
  GriddingParams foldParams = params;
  if (!fullScans)
    foldParams.maxSweeps = std::max(50, params.maxSweeps / 2); // QC 快档
  std::vector<ScatterPoint> subset;
  subset.reserve(points.size() - 1);
  std::vector<float> grid;
  std::vector<double> resid;
  for (int fold = 0; fold < static_cast<int>(foldIdx.size()); ++fold)
  {
    if (control.cancelRequested && control.cancelRequested())
      return fail(QStringLiteral("canceled"));
    subset.clear();
    const int hold = foldIdx[fold];
    for (int k = 0; k < static_cast<int>(points.size()); ++k)
      if (k != hold)
        subset.push_back(points[k]);
    QString solveErr;
    if (!solveMinimumCurvature(subset, geometry, foldParams, nullptr, &grid, nullptr,
                               &solveErr))
      return fail(QStringLiteral("fold %1 failed: %2").arg(fold).arg(solveErr));
    const double est = sampleBilinear(grid, geometry, points[hold].x, points[hold].y);
    if (std::isnan(est))
      continue; // 留出点落双线性窗边缘/屏障 → 该折不计（不伪造残差）
    resid.push_back(points[hold].z - est);
    res.residuals.push_back(static_cast<float>(points[hold].z - est));
    res.heldOutIndex.push_back(hold);
  }
  if (resid.empty())
    return fail(QStringLiteral("no fold produced a usable estimate"));
  double sum = 0, sumSq = 0, maxAbs = 0;
  for (double r : resid)
  {
    sum += r;
    sumSq += r * r;
    maxAbs = std::max(maxAbs, std::fabs(r));
  }
  res.folds = static_cast<int>(resid.size());
  res.mean = sum / resid.size();
  res.rms = std::sqrt(sumSq / resid.size());
  res.maxAbs = maxAbs;
  *out = std::move(res);
  return true;
}

} // namespace paleo::gridsolver
