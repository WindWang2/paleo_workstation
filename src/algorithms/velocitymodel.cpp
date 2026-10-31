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
  if (!std::isfinite(twtMs) || twtMs < 0.0 || !(v0 > 0.0))
    return qQNaN();
  if (twtMs == 0.0)
    return 0.0;
  const double u = k * twtMs / 2000.0;
  if (std::abs(u) < 1e-9)
    return v0 * twtMs / 2000.0;
  return v0 / k * std::expm1(u);
}

// V0-k 反闭合式 TWT(z) = 2000/k·ln(1 + k·z/v0)。
double v0kTwt(double v0, double k, double depthM)
{
  if (!std::isfinite(depthM) || depthM < 0.0 || !(v0 > 0.0))
    return qQNaN();
  if (depthM == 0.0)
    return 0.0;
  const double u = k * depthM / v0;
  // L8：k<0 时 v(z)=v0+k·z 在 z ≥ −v0/k 处非正，对数自变量 ≤0 → 显式 NaN（旧式给 −inf/NaN）。
  if (!(u > -1.0))
    return qQNaN();
  if (std::abs(u) < 1e-9)
    return 2000.0 * depthM / v0;
  return 2000.0 / k * std::log1p(u);
}

// V0-k 两点拟合（#718）：过原点恒速在两结点不共线时井上误差可达 km 级
// （首结点不在 SRD 时尤甚）。改以首结点为参考解
//   z₂ = z₁·(e^{a·t₂}−1)/(e^{a·t₁}−1),  a = k/2000 ≠ 0
// 再折算回过原点口径 v₀ = k·z₁/(e^{k·t₁/2000}−1)——折算后模型仍过原点，
// 且**精确过两结点**（位级锚点，fitRmsMs≈0）。首结点就在 SRD（t₁≤0）或
// z₁≤0 时过原点恒速即精确（无需求解）；折算出非正速度/速度在井段内非正时
// 回落过原点恒速（调用方按 V0 非正跳过该井）。
void fitV0kTwoKnots( const QVector<VelocityKnot> &knots, double *v0Out, double *kOut )
{
  const double t1 = knots.at( 0 ).twtMs, z1 = knots.at( 0 ).depthM;
  const double t2 = knots.at( 1 ).twtMs, z2 = knots.at( 1 ).depthM;
  const auto constantThroughOrigin = [&]() {
    *v0Out = 2000.0 * ( z2 - z1 ) / ( t2 - t1 );
    *kOut = 0.0;
  };
  if ( !( t1 > 0.0 ) || !( z1 > 0.0 ) || !( t2 > t1 ) || !std::isfinite( t1 ) ||
       !std::isfinite( z1 ) || !std::isfinite( t2 ) || !std::isfinite( z2 ) )
  {
    constantThroughOrigin(); // 首结点在 SRD 或数据退化：过原点恒速已精确/唯一可选
    return;
  }
  // g(a) = z₁·(e^{a·t₂}−1) − z₂·(e^{a·t₁}−1)；g(0)≡0（过原点恒速解），所求为
  // 另一非零根。g'(0) = z₁·t₂ − z₂·t₁ 的符号决定非零根在 a 的哪一侧。
  const double gPrime0 = z1 * t2 - z2 * t1;
  if ( std::abs( gPrime0 ) <= 1e-12 * ( std::abs( z1 * t2 ) + std::abs( z2 * t1 ) ) )
  {
    constantThroughOrigin(); // 两结点与原点共线：过原点恒速即精确解
    return;
  }
  const double side = gPrime0 < 0.0 ? 1.0 : -1.0; // g'(0)<0 → 根在正侧，反之负侧
  const auto g = [&]( double a ) {
    return z1 * std::expm1( a * t2 ) - z2 * std::expm1( a * t1 );
  };
  // 括号 + 二分：固定迭代次数（确定性，同输入同输出）。
  double lo = 0.0, hi = side * 1e-6;
  double flo = 0.0, fhi = g( hi );
  int expand = 0;
  while ( fhi * flo >= 0.0 && expand < 200 )
  {
    lo = hi;
    flo = fhi;
    hi *= 2.0;
    fhi = g( hi );
    ++expand;
  }
  if ( fhi * flo >= 0.0 || !std::isfinite( fhi ) || !std::isfinite( flo ) )
  {
    constantThroughOrigin(); // 未括号到异号根（极端数据）：回落旧口径
    return;
  }
  for ( int iter = 0; iter < 200; ++iter )
  {
    const double mid = 0.5 * ( lo + hi );
    const double fm = g( mid );
    if ( fm == 0.0 )
    {
      lo = hi = mid;
      break;
    }
    if ( fm * flo < 0.0 )
      hi = mid;
    else
    {
      lo = mid;
      flo = fm;
    }
  }
  const double a = 0.5 * ( lo + hi );
  const double k = 2000.0 * a;
  const double u = k * t1 / 2000.0; // = a·t1
  // v₀ = k·z₁/(e^{k·t₁/2000}−1)；|u| 极小时用极限 2000·z₁/t₁（数值消减）。
  const double v0 = std::abs( u ) < 1e-9 ? 2000.0 * z1 / t1 : k * z1 / std::expm1( u );
  // 速度必须在井段内恒正（对数/指数定义域）；否则该两点无法用合法 V0-k 表示。
  if ( !( v0 > 0.0 ) || !std::isfinite( v0 ) || !( v0 + k * std::max( z1, z2 ) > 0.0 ) )
  {
    constantThroughOrigin();
    return;
  }
  *v0Out = v0;
  *kOut = k;
}

// V0-k 拟合：TWT 域 2 参数 Gauss-Newton，残差 r_i = T(z_i) − t_i（T 为反闭
// 合式）。层间速度是 1/v 的调和型层平均，对瞬时速度线做线性 LS 有系统偏差
// （积分关系不闭合）；直接在时深对上解 v(z)=V0+k·z 才与正闭合式自洽。
// 两个控制点不强解梯度：以首结点为参考解出精确过两结点的 V0-k（#718），
// 参考解不成立时回落 V0=Vint、k=0（过原点恒速）。
// 固定迭代上限的确定性算法——同输入同输出（序列化/幂等语义依赖）。
void fitV0k(const QVector<VelocityKnot> &knots, double *v0Out, double *kOut)
{
  if (knots.size() < 3)
  {
    fitV0kTwoKnots( knots, v0Out, kOut );
    return;
  }
  // 初值：常速 LS（t = 2000z/v0 对 v0 的最小二乘）。
  double szz = 0.0, szt = 0.0, zMax = 0.0;
  for (const VelocityKnot &kn : knots)
  {
    if (!(kn.depthM > 0.0) || !std::isfinite(kn.twtMs)) // #838：z<=0 结点使 log1p/除零无定义
      continue;
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
      if (!(z > 0.0) || !std::isfinite(kn.twtMs))
        continue;
      const double v = v0 + k * z;
      const double r = v0kTwt(v0, k, z) - kn.twtMs;
      const double dTdv0 = -2000.0 * z / (v0 * v);
      const double dTdk = std::abs(k) < 1e-12
          ? -1000.0 * z * z / (v0 * v0)
          : -2000.0 / (k * k) * std::log1p(k * z / v0) + 2000.0 * z / (k * v);
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

// power-2 IDW（w=1/d²，命中即取）。
// M4：perWell 给 NaN（结点范围外）的井，若提供 extended 则改用其外延值参与
// 加权——但仅当至少一口井在范围内有答案时；全部井都在范围外仍返回 NaN
// （不外推整个模型）。旧实现直接剔除该井，井结点结束处权重突变，深度可跳
// 变上千米且随 TWT 非单调。
double idwOverWells(const QVector<VelocityModel::Well> &wells, double x, double y, double twtMs,
                    const VelocityModel *model,
                    double (VelocityModel::*perWell)(const VelocityModel::Well &, double) const,
                    double (VelocityModel::*extended)(const VelocityModel::Well &, double) const = nullptr)
{
  double weightSum = 0.0, valueSum = 0.0;
  bool anyInRange = false;
  double extWeightSum = 0.0, extValueSum = 0.0;
  double hitExtended = qQNaN();
  for (const VelocityModel::Well &w : wells)
  {
    const double dx = w.x - x, dy = w.y - y;
    const double d2 = dx * dx + dy * dy;
    double v = (model->*perWell)(w, twtMs);
    if (std::isnan(v))
    {
      if (!extended)
        continue; // 样点范围外：不参与加权，也不阻断其他井
      v = (model->*extended)(w, twtMs);
      if (std::isnan(v))
        continue;
      // BIZ-13 同型防线（paleoalgorithms 已修，此处对齐口径）：0 < d2 < 极小
      // 时 1/d² 溢出 → 加权和 ∞/∞ = NaN——按命中井位处理而非浮点加权。
      if (d2 < 1e-12)
      {
        hitExtended = v;
        continue;
      }
      const double wgt = 1.0 / d2;
      extWeightSum += wgt;
      extValueSum += wgt * v;
      continue;
    }
    anyInRange = true;
    if (d2 < 1e-12)
      return v; // 命中井位 → 锚点精确（不经浮点加权，BIZ-13 口径）
    const double wgt = 1.0 / d2;
    weightSum += wgt;
    valueSum += wgt * v;
  }
  if (!anyInRange)
    return qQNaN();
  if (!std::isnan(hitExtended))
    return hitExtended; // 命中井位（该井外延）：与井位处单井外延一致
  weightSum += extWeightSum;
  valueSum += extValueSum;
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

double VelocityModel::wellDepthForTwtExtended(const Well &w, double twtMs) const
{
  if (m_type == ModelType::V0kLinear)
    return v0kDepth(w.v0, w.k, twtMs);
  const auto &kn = w.knots;
  if (!std::isfinite(twtMs) || twtMs < 0.0 || kn.size() < 2)
    return qQNaN();
  const int n = int(kn.size());
  if (twtMs > kn.last().twtMs)
  {
    const double vint = (kn.at(n - 1).depthM - kn.at(n - 2).depthM) / (kn.at(n - 1).twtMs - kn.at(n - 2).twtMs);
    return kn.last().depthM + (twtMs - kn.last().twtMs) * vint;
  }
  if (twtMs < kn.first().twtMs)
  {
    // #719：首结点之上不外延「首层区间速度」——t1 相对 (0,0) 的深时比常与该层
    // 不同，外延距离一大就把 TVD 外延成负数（速度模型向首结点之上外延时 TVD
    // 为负）。改按 (0,0)-(t1,z1) 线性插值（地表到首结点的常速语义），并留
    // z<0 守卫：数值上仍为负则如实 NaN，不输出负深度。
    const double t1 = kn.first().twtMs, z1 = kn.first().depthM;
    if ( !( t1 > 0.0 ) )
      return qQNaN(); // 首结点 TWT 非正：无 (0,0) 参考可插值
    const double z = twtMs * z1 / t1;
    return z < 0.0 ? qQNaN() : z;
  }
  return wellDepthForTwt(w, twtMs);
}

double VelocityModel::wellVelocityAtExtended(const Well &w, double twtMs) const
{
  if (m_type == ModelType::V0kLinear)
    return wellVelocityAt(w, twtMs);
  const auto &kn = w.knots;
  if (!std::isfinite(twtMs) || twtMs < 0.0 || kn.size() < 2)
    return qQNaN();
  const int n = int(kn.size());
  if (twtMs > kn.last().twtMs)
    return 2000.0 * (kn.at(n - 1).depthM - kn.at(n - 2).depthM) / (kn.at(n - 1).twtMs - kn.at(n - 2).twtMs);
  if (twtMs < kn.first().twtMs)
  {
    // 与 wellDepthForTwtExtended 的 (0,0)-(t1,z1) 插值同口径：首结点之上的
    // 有效速度是地表到首结点的平均速度（2000·z1/t1），不再是首层区间速度
    // ——否则深度与速度两条外延曲线在首结点之上互相矛盾（#719）。
    const double t1 = kn.first().twtMs, z1 = kn.first().depthM;
    return t1 > 0.0 ? 2000.0 * z1 / t1 : qQNaN();
  }
  return wellVelocityAt(w, twtMs);
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
  return idwOverWells(m_wells, x, y, twtMs, this, &VelocityModel::wellDepthForTwt,
                      &VelocityModel::wellDepthForTwtExtended);
}

double VelocityModel::velocityAt(double x, double y, double twtMs) const
{
  return idwOverWells(m_wells, x, y, twtMs, this, &VelocityModel::wellVelocityAt,
                      &VelocityModel::wellVelocityAtExtended);
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
                                       const double gt[6], double nodata,
                                       const std::function<bool()> &cancelled)
{
  DepthGridResult r;
  r.depthM.resize(rows * cols);
  std::fill(r.depthM.begin(), r.depthM.end(), qQNaN());
  // 尺寸契约：timeMs 必须就是 rows×cols。旧式用 QVector::value() 静默取 0.0f，
  // 越界像元被当成合法 TWT=0ms 换算成深度 0 写进输出——错误被吞成貌似合理
  // 的错值。不符即整体拒绝（保持全 NaN），不发布半真半假的深度面。
  if (timeMs.size() != qsizetype(rows) * cols)
  {
    r.nodataCells = qsizetype(rows) * cols; // 没有一格被转换，如实计数
    return r;
  }
  for (int row = 0; row < rows; ++row)
  {
    if (cancelled && cancelled())
    {
      r.cancelled = true;
      return r; // 半途结果：统计不完整，调用方必须弃用
    }
    for (int col = 0; col < cols; ++col)
    {
      const int idx = row * cols + col;
      const float t = timeMs.value(idx);
      // #165：像元是 float，nodata 按 float 口径比较（1e30 等非 float 可表示值）。
      if (!std::isfinite(t) || t == static_cast<float>(nodata))
      {
        ++r.nodataCells;
        continue;
      }
      // #834：完整 GDAL 仿射（含旋转项 gt[2]/gt[4]），原生旋转测网不可只用 gt[1]/gt[5]。
      const double cx = gt[0] + (col + 0.5) * gt[1] + (row + 0.5) * gt[2];
      const double cy = gt[3] + (col + 0.5) * gt[4] + (row + 0.5) * gt[5];
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
