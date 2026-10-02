// 层：数据
#pragma once
#include <QString>
#include <QVector>
#include <QtGlobal> // qQNaN

class QJsonObject;

// algorithms/ — 时深转换速度模型核（goal/time-depth-velocity）。
// 由井控制点（分层对/校验炮时深表）拟合空间速度模型，供层位/剖面时间域→
// 深度域换算。纯数值（QtCore 容器/JSON，无 QtWidgets/GIS），两种模型语义：
//   · IntervalAverage 层间平均速度：井内相邻控制点分段线性（层内 Vint 恒定，
//     Vint_i = 2000·ΔTVD_i/ΔTWT_i）。插值语义——控制点（锚点）处精确命中
//     存档深度，样点范围外不外推（同 TimeDepthTool/TimeDepthModel 纪律）。
//   · V0kLinear 线性速度函数 v(z) = V0 + k·z（Sheriff, Encyclopedic Dictionary
//     of Applied Geophysics, "velocity function"；V0:m/s, k:1/s）：对层间速度-
//     层中点深度最小二乘拟合。模型语义——闭合式 z(twt) = V0/k·(e^{k·twt/2000}−1)
//     处处可算（允许超出样点范围外推，拟合残差以 fitRmsMs 如实记录，锚点
//     不保证精确命中是回归模型的本性，测试钉住该口径）。
// 空间查询：逐井算一维答案后 power-2 IDW（w=1/d²，命中井位即取——与
// paleo:paleo_constraint_idw 同权重契约），某井样点范围外不参与该点加权。
// 序列化 JSON（format=paleo-velocity-model）；不写时间戳——重复加载/转换幂等。
namespace paleo::velmodel {

// 单井控制点对：TWT(ms) ↔ TVD(m)。文件序即深度序（调用方不排序，本核校验）。
struct VelocityKnot
{
  double twtMs = 0.0;
  double depthM = 0.0;
  QString topName; // 分层名（校验炮来源时为空）
};

struct VelocityWellControl
{
  QString wellId;
  double x = qQNaN(), y = qQNaN(); // 井口地图坐标（缺 → 该井不参与空间查询）
  QVector<VelocityKnot> knots;
};

enum class ModelType { IntervalAverage, V0kLinear };

inline QString modelTypeId(ModelType t)
{
  return t == ModelType::V0kLinear ? QStringLiteral("v0k_linear")
                                   : QStringLiteral("interval_average");
}

class VelocityModel
{
public:
  struct Well
  {
    QString id;
    double x = qQNaN(), y = qQNaN();
    QVector<VelocityKnot> knots; // 原始控制点（两种模型都保留：provenance/重建）
    double v0 = 0.0, k = 0.0;    // V0kLinear 拟合参数（IntervalAverage 恒 0）
    double fitRmsMs = qQNaN();   // 拟合残差 RMS(ms)：T(z_i) 闭合式 vs 控制点 TWT
  };

  // 拟合入口。逐井校验（≥2 控制点、TWT/TVD 文件序双严格递增、坐标有限）；
  // 不合格井跳过并记 notes（不静默丢弃）；全部不合格 → false + error。
  static VelocityModel fit(const QVector<VelocityWellControl> &controls,
                           ModelType type, QString *error = nullptr);

  ModelType type() const { return m_type; }
  const QVector<Well> &wells() const { return m_wells; }
  const QStringList &notes() const { return m_notes; }
  bool isValid() const { return !m_wells.isEmpty(); }

  // (x,y,twt) → TVD(m)。无可用井答案（范围外/全 NaN）→ NaN。
  double depthForTwt(double x, double y, double twtMs) const;
  // (x,y,twt) → 瞬时区间速度(m/s)：IntervalAverage=包含层的 Vint；
  // V0kLinear = v0 + k·z(twt)。无答案 → NaN。
  double velocityAt(double x, double y, double twtMs) const;
  // (x,y,twt) → 自地表平均速度(m/s) = 2·z(twt)/twt。twt≤0 或无答案 → NaN。
  double averageVelocityAt(double x, double y, double twtMs) const;

  // 单井一维查询（不经空间加权；与空间查询在井位处逐位一致）。
  double wellDepthForTwt(const Well &w, double twtMs) const;
  double wellVelocityAt(const Well &w, double twtMs) const;
  // V0kLinear 反闭合式 TWT(ms) ← TVD(m)（残差度量用）；IntervalAverage 沿
  // 分段线性反解。范围语义同正向（interval 不外推 / v0k 允许）。
  double wellTwtForDepth(const Well &w, double depthM) const;

  QJsonObject toJson() const;
  static VelocityModel fromJson(const QJsonObject &obj, QString *error = nullptr);

private:
  ModelType m_type = ModelType::IntervalAverage;
  QVector<Well> m_wells;
  QStringList m_notes;
};

// ---- 时间栅格 → 深度栅格执行器（逐像元中心查模型） ----------------------------
// gt[6] 为北向上 GDAL geotransform（同 BinnedHorizon/厚度链口径）：
// cx = gt[0] + (col+0.5)·gt[1]；cy = gt[3] + (row+0.5)·gt[5]。
struct DepthGridResult
{
  QVector<float> depthM;      // row-major 同输入布局；无答案像元 = NaN
  int convertedCells = 0;
  int nodataCells = 0;        // 输入 nodata/非有限 → 透传 NaN（不臆造深度）
  int outsideModelCells = 0;  // 输入有值但模型无答（井样点范围外）
};

DepthGridResult convertTimeGridToDepth(const VelocityModel &model,
                                       const QVector<float> &timeMs, int rows, int cols,
                                       const double gt[6], double nodata);

} // namespace paleo::velmodel
