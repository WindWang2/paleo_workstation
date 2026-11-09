// 层：数据
#pragma once
#include <QHash>
#include <QString>
#include <QVector>

#include "domain/surveygridgeometry.h"
#include "domain/wellrecords.h"

namespace TimeDepthTool { struct TdResult; }

// services/ — §40 SeismicMapLink 的两个新建依赖（PALEO_QGIS_PLAN §1314 E1
// 决议：速度模型与 line-geometry↔CDP 映射此前被当作「现有组件」，实为新建）。
// 纯计算、无 IO：几何参数来自层位文件头（horizonbinner 解析）或 catalog 冻结
// 的 survey 实体；TD 表由调用方（ProjectDataFacade）供给。三条契约：
//   · 超出测网 → 明确失败 + 原因，绝不夹取到网边界；
//   · CDP 映射按测线分段线性，非仿射道头观测整线拒绝；
//   · depth↔TWT 双向同 TimeDepthTool 契约（文件顺序、不排序、不外推）。

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
