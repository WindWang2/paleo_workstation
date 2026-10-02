// 层：数据
#include "velocitymodel.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace paleo::velmodel {

namespace {

// 文件序双严格递增（TWT 与 TVD 同时）——不排序修补，违例井整体拒绝。
bool knotsStrictlyIncreasing(const QVector<VelocityKnot> &knots)
{
  for (int i = 1; i < knots.size(); ++i)
  {
    if (!(knots[i].twtMs > knots[i - 1].twtMs) || !(knots[i].depthM > knots[i - 1].depthM))
      return false;
  }
  return true;
}

// V0-k 正闭合式 z(twt)。u = k·twt/2000 很小时 (e^u−1)/u 数值消减 → 用常速极限。
double v0kDepth(double v0, double k, double twtMs)
{
  if (!(twtMs > 0.0))
    return qQNaN();
  const double u = k * twtMs / 2000.0;
  if (std::abs(u) < 1e-9)
    return v0 * twtMs / 2000.0;
  return v0 / k * std::expm1(u);
}

// V0-k 反闭合式 TWT(z) = 2000/k·ln(1 + k·z/v0)。
double v0kTwt(double v0, double k, double depthM)
{
  if (!(depthM > 0.0) || !(v0 > 0.0))
    return qQNaN();
  const double u = k * depthM / v0;
  if (std::abs(u) < 1e-9)
    return 2000.0 * depthM / v0;
  return 2000.0 / k * std::log1p(u);
}

// V0-k 拟合：TWT 域 2 参数 Gauss-Newton，残差 r_i = T(z_i) − t_i（T 为反闭
// 合式）。层间速度是 1/v 的调和型层平均，对瞬时速度线做线性 LS 有系统偏差
// （积分关系不闭合）；直接在时深对上解 v(z)=V0+k·z 才与正闭合式自洽。
// 单层井（2 控制点）不强解梯度：V0=Vint、k=0（常速，诚实口径）。
// 固定迭代上限的确定性算法——同输入同输出（序列化/幂等语义依赖）。
void fitV0k(const QVector<VelocityKnot> &knots, double *v0Out, double *kOut)
{
  if (knots.size() < 3)
  {
    const double dt = knots.at(1).twtMs - knots.at(0).twtMs;
    const double dz = knots.at(1).depthM - knots.at(0).depthM;
    *v0Out = 2000.0 * dz / dt;
    *kOut = 0.0;
    return;
  }
  // 初值：常速 LS（t = 2000z/v0 对 v0 的最小二乘）。
  double szz = 0.0, szt = 0.0, zMax = 0.0;
  for (const VelocityKnot &kn : knots)
  {
    szz += kn.depthM * kn.depthM;
    szt += kn.depthM * kn.twtMs;
    zMax = std::max(zMax, kn.depthM);
  }
  double v0 = 2000.0 * szz / szt;
  double k = 0.0;
  if (!(v0 > 0.0) || !std::isfinite(v0))
  {
    *v0Out = 0.0; // 调用方按「V0 非正」跳过该井
    *kOut = 0.0;
    return;
  }
  for (int iter = 0; iter < 60; ++iter)
  {
    double a11 = 0.0, a12 = 0.0, a22 = 0.0, b1 = 0.0, b2 = 0.0;
    for (const VelocityKnot &kn : knots)
    {
      const double z = kn.depthM;
      const double v = v0 + k * z;
      const double r = v0kTwt(v0, k, z) - kn.twtMs;
      const double dTdv0 = -2000.0 * z / (v0 * v);
      const double dTdk = std::abs(k) < 1e-12
          ? -1000.0 * z * z / (v0 * v0)
          : -2000.0 / (k * k) * std::log1p(k * z / v0) + 2000.0 * z / (v0 * v);
      a11 += dTdv0 * dTdv0; a12 += dTdv0 * dTdk; a22 += dTdk * dTdk;
      b1 += dTdv0 * r; b2 += dTdk * r;
    }
    const double det = a11 * a22 - a12 * a12;
    if (!std::isfinite(det) || std::abs(det) < 1e-30)
      break;
    const double dv0 = (-b1 * a22 + b2 * a12) / det;
    const double dk = (b1 * a12 - b2 * a11) / det;
    // 阻尼回溯：V0>0 且全井段 v(z)>0（对数/指数定义域），否则步长减半。
    double scale = 1.0;
    bool stepped = false;
    for (int half = 0; half < 30; ++half)
    {
      const double nv = v0 + scale * dv0;
      const double nk = k + scale * dk;
      if (nv > 0.0 && nv + nk * zMax > 0.0 && std::isfinite(nv) && std::isfinite(nk))
      {
        const double relStep = std::abs(scale * dv0) / nv + std::abs(scale * dk) * zMax / nv;
        v0 = nv; k = nk; stepped = true;
        if (relStep < 1e-12)
          iter = 60; // 已收敛
        break;
      }
      scale *= 0.5;
    }
    if (!stepped)
      break;
  }
  *v0Out = v0;
  *kOut = k;
}

// 结点按键二分（upper_bound 语义：首个严格大于 key 的下标；结点严格递增）。
// 校验炮表 ~500 结点/井，线性扫是 20 井×263k 像元转换的显著热点。
int upperBoundKnot(const QVector<VelocityKnot> &knots, double key, bool byTwt)
{
  int lo = 0, hi = knots.size();
  while (lo < hi)
  {
    const int mid = lo + (hi - lo) / 2;
    const double v = byTwt ? knots.at(mid).twtMs : knots.at(mid).depthM;
    if (v <= key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

// power-2 IDW（w=1/d²，命中即取；NaN 答案的井不参与该点加权）。
double idwOverWells(const QVector<VelocityModel::Well> &wells, double x, double y, double twtMs,
                    const VelocityModel *model,
                    double (VelocityModel::*perWell)(const VelocityModel::Well &, double) const)
{
  double weightSum = 0.0, valueSum = 0.0;
  for (const VelocityModel::Well &w : wells)
  {
    const double dx = w.x - x, dy = w.y - y;
    const double d2 = dx * dx + dy * dy;
    const double v = (model->*perWell)(w, twtMs);
    if (std::isnan(v))
      continue; // 样点范围外：不参与加权，也不阻断其他井
    if (d2 == 0.0)
      return v; // 命中井位 → 锚点精确（不经浮点加权）
    const double wgt = 1.0 / d2;
    weightSum += wgt;
    valueSum += wgt * v;
  }
  return weightSum > 0.0 ? valueSum / weightSum : qQNaN();
}

} // namespace

VelocityModel VelocityModel::fit(const QVector<VelocityWellControl> &controls,
                                 ModelType type, QString *error)
{
  VelocityModel m;
  m.m_type = type;
  for (const VelocityWellControl &c : controls)
  {
    if (c.knots.size() < 2)
    {
      m.m_notes << QStringLiteral("跳过 %1：控制点不足 2 个").arg(c.wellId);
      continue;
    }
    if (!knotsStrictlyIncreasing(c.knots))
    {
      m.m_notes << QStringLiteral("跳过 %1：TWT/TVD 文件序非双严格递增").arg(c.wellId);
      continue;
    }
    if (!std::isfinite(c.x) || !std::isfinite(c.y))
    {
      m.m_notes << QStringLiteral("跳过 %1：井位坐标缺失").arg(c.wellId);
      continue;
    }
    Well w;
    w.id = c.wellId;
    w.x = c.x;
    w.y = c.y;
    w.knots = c.knots;
    if (type == ModelType::V0kLinear)
    {
      fitV0k(c.knots, &w.v0, &w.k);
      if (!(w.v0 > 0.0))
      {
        m.m_notes << QStringLiteral("跳过 %1：V0-k 拟合非正速度").arg(c.wellId);
        continue;
      }
      double resid2 = 0.0;
      for (const VelocityKnot &kn : c.knots)
      {
        const double dt = v0kTwt(w.v0, w.k, kn.depthM) - kn.twtMs;
        resid2 += dt * dt;
      }
      w.fitRmsMs = std::sqrt(resid2 / c.knots.size());
    }
    m.m_wells.append(w);
  }
  if (m.m_wells.isEmpty())
  {
    if (error)
      *error = QStringLiteral("速度模型拟合失败：无合格控制井（%1 条备注）")
                   .arg(m.m_notes.size());
  }
  return m;
}

double VelocityModel::wellDepthForTwt(const Well &w, double twtMs) const
{
  if (m_type == ModelType::V0kLinear)
    return v0kDepth(w.v0, w.k, twtMs); // 回归模型语义：闭合式处处可算
  const auto &kn = w.knots;
  if (!(twtMs >= kn.first().twtMs) || !(twtMs <= kn.last().twtMs))
    return qQNaN(); // 插值语义：不外推
  // 结点 TWT 严格递增（fit 校验）→ 二分找包含段；等值命中左结点（f=0）。
  const int seg = qBound(1, upperBoundKnot(kn, twtMs, true), int(kn.size()) - 1);
  const double dt = kn.at(seg).twtMs - kn.at(seg - 1).twtMs;
  const double f = (twtMs - kn.at(seg - 1).twtMs) / dt;
  // 锚点 twtMs == kn[seg-1].twtMs → f=0 → 原样返回存档深度（位级精确）。
  return kn.at(seg - 1).depthM + f * (kn.at(seg).depthM - kn.at(seg - 1).depthM);
}

double VelocityModel::wellVelocityAt(const Well &w, double twtMs) const
{
  const auto &kn = w.knots;
  if (m_type == ModelType::V0kLinear)
    return w.v0 + w.k * v0kDepth(w.v0, w.k, twtMs);
  if (!(twtMs >= kn.first().twtMs) || twtMs > kn.last().twtMs)
    return qQNaN();
  const int seg = qBound(1, upperBoundKnot(kn, twtMs, true), int(kn.size()) - 1);
  const double dt = kn.at(seg).twtMs - kn.at(seg - 1).twtMs;
  return 2000.0 * (kn.at(seg).depthM - kn.at(seg - 1).depthM) / dt;
}

double VelocityModel::wellTwtForDepth(const Well &w, double depthM) const
{
  const auto &kn = w.knots;
  if (m_type == ModelType::V0kLinear)
    return v0kTwt(w.v0, w.k, depthM);
  if (!(depthM >= kn.first().depthM) || !(depthM <= kn.last().depthM))
    return qQNaN();
  const int seg = qBound(1, upperBoundKnot(kn, depthM, false), int(kn.size()) - 1);
  const double dz = kn.at(seg).depthM - kn.at(seg - 1).depthM;
  const double f = (depthM - kn.at(seg - 1).depthM) / dz;
  return kn.at(seg - 1).twtMs + f * (kn.at(seg).twtMs - kn.at(seg - 1).twtMs);
}

double VelocityModel::depthForTwt(double x, double y, double twtMs) const
{
  return idwOverWells(m_wells, x, y, twtMs, this, &VelocityModel::wellDepthForTwt);
}

double VelocityModel::velocityAt(double x, double y, double twtMs) const
{
  return idwOverWells(m_wells, x, y, twtMs, this, &VelocityModel::wellVelocityAt);
}

double VelocityModel::averageVelocityAt(double x, double y, double twtMs) const
{
  if (!(twtMs > 0.0))
    return qQNaN();
  const double z = depthForTwt(x, y, twtMs);
  if (std::isnan(z))
    return qQNaN();
  return 2000.0 * z / twtMs; // m/s（twt 毫秒 → 秒）
}

// ---- 序列化 ------------------------------------------------------------------

QJsonObject VelocityModel::toJson() const
{
  QJsonArray wellArr;
  for (const Well &w : m_wells)
  {
    QJsonArray knotArr;
    for (const VelocityKnot &kn : w.knots)
    {
      QJsonObject ko;
      ko.insert(QStringLiteral("twtMs"), kn.twtMs);
      ko.insert(QStringLiteral("depthM"), kn.depthM);
      if (!kn.topName.isEmpty())
        ko.insert(QStringLiteral("top"), kn.topName);
      knotArr.append(ko);
    }
    QJsonObject wo;
    wo.insert(QStringLiteral("id"), w.id);
    wo.insert(QStringLiteral("x"), w.x);
    wo.insert(QStringLiteral("y"), w.y);
    wo.insert(QStringLiteral("knots"), knotArr);
    if (m_type == ModelType::V0kLinear)
    {
      wo.insert(QStringLiteral("v0"), w.v0);
      wo.insert(QStringLiteral("k"), w.k);
      wo.insert(QStringLiteral("fitRmsMs"), w.fitRmsMs);
    }
    wellArr.append(wo);
  }
  QJsonObject root;
  root.insert(QStringLiteral("format"), QStringLiteral("paleo-velocity-model"));
  root.insert(QStringLiteral("version"), 1);
  root.insert(QStringLiteral("type"), modelTypeId(m_type));
  root.insert(QStringLiteral("wells"), wellArr);
  if (!m_notes.isEmpty())
  {
    QJsonArray noteArr;
    for (const QString &n : m_notes)
      noteArr.append(n);
    root.insert(QStringLiteral("notes"), noteArr);
  }
  return root;
}

VelocityModel VelocityModel::fromJson(const QJsonObject &obj, QString *error)
{
  const auto fail = [error](const QString &why) {
    if (error)
      *error = why;
    return VelocityModel();
  };
  if (obj.value(QStringLiteral("format")).toString() != QLatin1String("paleo-velocity-model"))
    return fail(QStringLiteral("不是 paleo-velocity-model 文档"));
  if (obj.value(QStringLiteral("version")).toInt() != 1)
    return fail(QStringLiteral("不支持的版本 %1").arg(obj.value(QStringLiteral("version")).toInt()));
  const QString typeStr = obj.value(QStringLiteral("type")).toString();
  const ModelType type = typeStr == QLatin1String("v0k_linear") ? ModelType::V0kLinear
                              : typeStr == QLatin1String("interval_average") ? ModelType::IntervalAverage
                                                                             : ModelType::IntervalAverage;
  if (typeStr != modelTypeId(type))
    return fail(QStringLiteral("未知模型类型 %1").arg(typeStr));

  VelocityModel m;
  m.m_type = type;
  const QJsonArray wellArr = obj.value(QStringLiteral("wells")).toArray();
  for (const auto &wv : wellArr)
  {
    const QJsonObject wo = wv.toObject();
    Well w;
    w.id = wo.value(QStringLiteral("id")).toString();
    w.x = wo.value(QStringLiteral("x")).toDouble();
    w.y = wo.value(QStringLiteral("y")).toDouble();
    const QJsonArray knotArr = wo.value(QStringLiteral("knots")).toArray();
    for (const auto &kv : knotArr)
    {
      const QJsonObject ko = kv.toObject();
      VelocityKnot kn;
      kn.twtMs = ko.value(QStringLiteral("twtMs")).toDouble();
      kn.depthM = ko.value(QStringLiteral("depthM")).toDouble();
      kn.topName = ko.value(QStringLiteral("top")).toString();
      w.knots.append(kn);
    }
    if (w.id.isEmpty() || w.knots.size() < 2 || !std::isfinite(w.x) || !std::isfinite(w.y) ||
        !knotsStrictlyIncreasing(w.knots))
      return fail(QStringLiteral("井 %1 控制数据不合法").arg(w.id));
    if (type == ModelType::V0kLinear)
    {
      w.v0 = wo.value(QStringLiteral("v0")).toDouble();
      w.k = wo.value(QStringLiteral("k")).toDouble();
      w.fitRmsMs = wo.value(QStringLiteral("fitRmsMs")).toDouble();
      if (!(w.v0 > 0.0))
        return fail(QStringLiteral("井 %1 V0 非正").arg(w.id));
    }
    m.m_wells.append(w);
  }
  if (m.m_wells.isEmpty())
    return fail(QStringLiteral("文档无合格控制井"));
  const QJsonArray noteArr = obj.value(QStringLiteral("notes")).toArray();
  for (const auto &nv : noteArr)
    m.m_notes << nv.toString();
  return m;
}

// ---- 栅格执行器 ----------------------------------------------------------------

DepthGridResult convertTimeGridToDepth(const VelocityModel &model,
                                       const QVector<float> &timeMs, int rows, int cols,
                                       const double gt[6], double nodata)
{
  DepthGridResult r;
  r.depthM.resize(rows * cols);
  std::fill(r.depthM.begin(), r.depthM.end(), qQNaN());
  for (int row = 0; row < rows; ++row)
  {
    const double cy = gt[3] + (row + 0.5) * gt[5];
    for (int col = 0; col < cols; ++col)
    {
      const int idx = row * cols + col;
      const float t = timeMs.value(idx);
      if (!std::isfinite(t) || t == nodata)
      {
        ++r.nodataCells;
        continue;
      }
      const double cx = gt[0] + (col + 0.5) * gt[1];
      const double z = model.depthForTwt(cx, cy, static_cast<double>(t));
      if (std::isnan(z))
      {
        ++r.outsideModelCells;
        continue;
      }
      r.depthM[idx] = static_cast<float>(z);
      ++r.convertedCells;
    }
  }
  return r;
}

} // namespace paleo::velmodel
