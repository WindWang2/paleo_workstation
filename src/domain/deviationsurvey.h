// 层：数据
#pragma once

#include <QString>
#include <QVector>

#include <limits>
#include <optional>

// domain/deviationsurvey — 测斜站表 → 三维轨迹（最小曲率法，行业标准）。
//
// 站点契约：MD(米) + 井斜角(°, [0,180]) + 方位角(°, 井北起顺时针)。站表按
// MD 排序后须严格递增；位移分量 north/east 相对井口（正北/正东），tvd 为
// 垂深（正下）。单分支井——侧钻/分支显式拒绝（递延，见 docs/progress）。
//
// 最小曲率（每段，站 1→站 2，弧长 ΔMD）：
//   cos β = cos i1·cos i2 + sin i1·sin i2·cos(ΔA)      （全狗腿角，含方位）
//   RF    = (2/β)·tan(β/2)                              （β→0 时 RF→1）
//   Δtvd   = ΔMD/2 · (cos i1 + cos i2) · RF
//   Δnorth = ΔMD/2 · (sin i1·cos A1 + sin i2·cos A2) · RF
//   Δeast  = ΔMD/2 · (sin i1·sin A1 + sin i2·sin A2) · RF
//
// 插值：站间任意 MD 沿该段最小曲率圆弧精确取点（切向在 t1→t2 平面内按
// 弧长 slerp，t=1 时与整段增量恒等；#168）。β≈π 的原地掉头段退回角度内插。
// 首站前按首站姿态直线（井口锚 (0,0,0)@MD0）；末站后按末站姿态直线延伸。
// 水平段（cos i → 0）外延无垂深增量。tvdToMd 返回首次到达该垂深的 MD；
// 轨迹到不了的垂深返回最接近处（最深点 MD），不外推猜值（#126）。
//
// 性能契约（方向 98 治本）：survey 自 fromStations 起不可变（无 mutator；
// 拷贝随行缓存），构造时预建逐段缓存——最小曲率圆弧框架 + 段内垂深极值
// 切分点（掉头段粗扫描同缓存）。pointAt 站点定位为二分查找（O(log N)），
// tvdToMd 的二分反解逐次调 pointAt——单次反解 O(段数 + 100·log N)，不再
// 每调用重算段几何（旧实现 O(站数)·三角 + 100·O(站数) 线性扫段）。

namespace paleo
{

namespace detail
{
// 逐段几何缓存（deviationsurvey.cpp 内部装配；此处定义以作 QVector 成员）。
// t1/n/beta 与圆弧框架同 #168 注；splitMd = 段内垂深极值 MD（无极值 → NaN，
// 含掉头段 64 等分粗扫描结果）。
struct DeviationSegment
{
  double t1[3] = {0, 0, 0};
  double n[3] = {0, 0, 0};
  double beta = 0.0;
  double splitMd = std::numeric_limits<double>::quiet_NaN();
  bool straight = false;
  bool reversal = false;
};
} // namespace detail


struct DeviationStation
{
  double md = 0.0;
  double inclinationDeg = 0.0;
  double azimuthDeg = 0.0;
};

struct TrajectoryPoint
{
  double md = 0.0;
  double tvd = 0.0;
  double north = 0.0; // 相对井口北向位移（米）
  double east = 0.0;  // 相对井口东向位移（米）
};

class WellDeviationSurvey
{
public:
  // 无效站表返回 nullopt 并写 error（空表 / 非有限值 / MD<0 / 井斜越界 /
  // 排序后 MD 重复）。方位角任意有限值（内部归一到 [0,360)）。
  static std::optional<WellDeviationSurvey> fromStations(
      QVector<DeviationStation> stations, QString *error = nullptr);

  bool isValid() const { return !m_stations.isEmpty(); }
  bool isEmpty() const { return m_stations.isEmpty(); }

  // 全站井斜≈0：垂井语义（tvdAt≡MD、位移≡0）。
  bool isVertical() const;

  const QVector<DeviationStation> &stations() const { return m_stations; }
  // 站点累计位置（与 stations 同长同序；含首站前直线锚定）。
  const QVector<TrajectoryPoint> &points() const { return m_points; }
  double totalDepth() const; // 末站 MD；空表 NaN

  TrajectoryPoint pointAt(double md) const;
  double tvdAt(double md) const { return pointAt(md).tvd; }
  double northAt(double md) const { return pointAt(md).north; }
  double eastAt(double md) const { return pointAt(md).east; }

  // tvdAt 的数值反解（同一最小曲率正函数，按 MD 顺序扫描单调片段后二分，
  // 往返双精度收敛）。契约（#126）：返回「首次到达」该垂深的最小 MD——上翘井
  // （井斜>90°）多解时取最浅 MD，返回值恒满足 tvdAt(md)≈tvd；表前/表后沿端站
  // 姿态直线反解；水平片段返回片段首 MD；轨迹到不了的垂深返回最深点 MD
  // （调用方可用 tvdAt(结果) 判断是否命中）。
  double tvdToMd(double tvd) const;

private:
  QVector<DeviationStation> m_stations;
  QVector<TrajectoryPoint> m_points;
  // 逐段缓存（fromStations 预建；与 stations 同步不可变，见性能契约注释）。
  QVector<detail::DeviationSegment> m_segments;
};

} // namespace paleo
