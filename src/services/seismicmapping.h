// 层：数据
#pragma once
#include <QHash>
#include <QString>
#include <QVector>

#include "../io/horizonbinner.h" // HorizonHeader（P1/P2/P3 + Grid_size）
#include "../io/timedeptool.h"   // TimeDepthTable/TdResult/TdStatus 契约

// services/ — §40 SeismicMapLink 的两个新建依赖（PALEO_QGIS_PLAN §1314 E1
// 决议：速度模型与 line-geometry↔CDP 映射此前被当作「现有组件」，实为新建）。
// 纯计算、无 IO：几何参数来自层位文件头（horizonbinner 解析）或 catalog 冻结
// 的 survey 实体；TD 表由调用方（ProjectDataFacade）供给。三条契约：
//   · 超出测网 → 明确失败 + 原因，绝不夹取到网边界；
//   · CDP 映射按测线分段线性，非仿射道头观测整线拒绝；
//   · depth↔TWT 双向同 TimeDepthTool 契约（文件顺序、不排序、不外推）。

// ---- 测网几何：map XY ↔ (inline, crossline) --------------------------------
// P1/P2/P3 是三个角锚 (inline, xline, x, y)：P1→P2 沿 crossline 方向、
// P2→P3 沿 inline 方向（binHorizon 已验证与 Grid_size 一致）。仿射：
//   x = p1x + a·(xl − p1Xline) + b·(inl − p1Inline)
//   y = p1y + c·(xl − p1Xline) + d·(inl − p1Inline)
// 支持斜测网（a..c 非零交叉项）；本工区为轴对齐（b=c=0）。
struct SurveyGridGeometry
{
  int inlineMin = 0, inlineMax = 0, xlineMin = 0, xlineMax = 0;
  double a = 0.0, b = 0.0, c = 0.0, d = 0.0; // 仿射系数（见上）
  double p1x = 0.0, p1y = 0.0;
  int p1Inline = 0, p1Xline = 0;
  bool valid = false;

  static SurveyGridGeometry fromHorizonHeader( const HorizonHeader &h );

  // 连续坐标正反解（不判范围；未构造 valid 时行为未定义——先查 valid）。
  void inlineXlineToXy( double inlineNo, double xline, double *x, double *y ) const;
  void xyToInlineXlineContinuous( double x, double y, double *inlineNo, double *xline ) const;

  // 整数化 + 范围门：最近格点语义——连续解四舍五入到最近 (inline, crossline)
  // 后必须在 [min,max] 内（含边界；网缘半道内自然归边缘道，与像元采样的
  // 半开区间口径互补：定位用最近道，取值用包含像元）。
  // 网外 → false + reason（含坐标与测网范围），输出参数不动——绝不夹取。
  bool xyToInlineXline( double x, double y, int *inlineNo, int *xline,
                        QString *reason = nullptr ) const;
  bool inlineXlineInside( int inlineNo, int xline ) const;
};

// ---- CDP 编号：逐 inline 的 (crossline ↔ CDP) 线性映射 ----------------------
// 道头观测 (inline, xline, cdp) 构造：每条 inline 内 CDP 随 xline 严格仿射
// （步长可逐线不同）；非线性（乱序/跳变）的线整线拒绝，查询给可读原因。
struct LineCdpMap
{
  struct LineAffine
  {
    qint64 baseCdp = 0;    // firstXline 处的 CDP
    double cdpPerXline = 0.0;
    int firstXline = 0;
  };

  QHash<int, LineAffine> lines;
  bool valid = true; // 至少有一条可用线即 true；逐线可用性查询时给原因

  static LineCdpMap fromObservations( const QVector<QVector<qint64>> &observations );

  bool cdpFor( int inlineNo, int xline, qint64 *cdp, QString *reason = nullptr ) const;
  bool xlineFor( int inlineNo, qint64 cdp, int *xline, QString *reason = nullptr ) const;
};

// ---- 井 TD 速度桥：depth ↔ TWT（双向，不外推） ------------------------------
// 正向 depth→TWT 直接走 TimeDepthTool（TVD 列优先、MD 兜底）；反向 TWT→depth
// 在此实现同一契约：行保持文件顺序、time 列作查找键须严格递增（否则
// NonMonotonic）、可用样点不足两个 NoTable、时间落在样点范围之外 OutOfRange、
// 相邻样点间线性插值。返回复用 TdResult（timeMs 字段承载深度，米）。
class VelocityModel
{
  public:
    void setWellTable( const QString &wellId, const TimeDepthTable &table );
    bool hasWell( const QString &wellId ) const;
    void clear();

    // depth（米）→ TWT（ms）。useMd=false 查 TVD 列；true 走 MD 兜底。
    TimeDepthTool::TdResult twtForDepth( const QString &wellId, double depth,
                                         bool useMd = false ) const;
    // TWT（ms）→ TVD（米；结果装在 TdResult::timeMs）。
    TimeDepthTool::TdResult depthForTwt( const QString &wellId, double timeMs ) const;

  private:
    QHash<QString, TimeDepthTable> m_wells;
};
