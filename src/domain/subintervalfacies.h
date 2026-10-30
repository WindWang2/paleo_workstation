// 层：数据
#pragma once
#include <QString>
#include <QVariantMap>
#include <QVector>

// domain/ — 子层段驱动的砂地比分类与井相/地震相证据纯值类型。
// 权威层段身份在 Data Service；本文件不持久化、不依赖 DS。
namespace paleo::strat
{

// 砂地比（0–1）粗分类：供井相证据与单因素分类图共用；阈值可配置。
struct SandRatioBins
{
  double mudMax = 0.25;       // (0, mudMax] → 泥质
  double muddySandMax = 0.50; // (mudMax, muddySandMax] → 泥质砂
  double sandMax = 0.75;      // (muddySandMax, sandMax] → 砂岩
  // (sandMax, 1] → 纯砂
};

struct SandRatioClass
{
  int code = 0; // 1..4；0 = 无效
  QString id;   // mud|muddy_sand|sand|clean_sand
  QString labelZh;
};

SandRatioClass classifySandRatio( double ratio, const SandRatioBins &bins = {} );

// 单井在子层段深度窗内的砂地比单因素（厚度/砂厚/比值）。
struct SandRatioMetrics
{
  double topMd = 0;
  double bottomMd = 0;
  double thicknessMd = 0;
  double sandThicknessMd = 0;
  double sandRatio = 0; // sand/thickness，物理域 [0,1]
  bool ok = false;
  QString reason;
  SandRatioClass faciesClass;
};

// 由已裁剪到 [top,bottom] 的砂岩厚度与层厚合成；thickness<=0 或砂厚越界 → 失败。
SandRatioMetrics makeSandRatioMetrics( double topMd, double bottomMd, double sandThicknessMd,
                                       const SandRatioBins &bins = {} );

// 父单元顶底 + 子层段序号（1..4）→ 等比初始深度窗（provisional）。
// 与 Data Service initial_equal 一致：半开 [top+(i-1)/4·H, top+i/4·H)，末段含底。
// 返回 false 当顶底无效或 index 越界。
bool provisionalSubRange( double parentTopMd, double parentBottomMd, int subIndex1to4, double *outTop,
                          double *outBottom );

// 地震相：明确区分「时间切片预测」与「层段综合结果」，禁止把单切片冒充整段。
enum class SeismicFaciesScope
{
  Slice,   // 二维时间切片预测
  Interval // 子层段掩膜内综合（可验证聚合）
};

QString seismicFaciesScopeId( SeismicFaciesScope scope );
QString seismicFaciesScopeLabelZh( SeismicFaciesScope scope );

struct SeismicSliceVote
{
  int classCode = 0;
  double weight = 1.0; // 通常为切片在掩膜内的有效样点数
};

struct SeismicIntervalAggregate
{
  int classCode = 0;
  double coverage = 0; // 有效权重 / 总权重
  bool ok = false;
  QString reason;
  QString strategyId = QStringLiteral( "weighted_mode" );
};

// minCoverage ∈ (0,1]；默认 0.5。权重和为 0 → 失败。
SeismicIntervalAggregate aggregateSeismicSliceVotes( const QVector<SeismicSliceVote> &votes,
                                                     double minCoverage = 0.5 );

// 编图证据行（禁止用 horizon 显示名做身份匹配；稳定键走 subIntervalId / assetRef）。
QVariantMap evidenceRowForBinding( const QString &kind, const QString &schemeId, const QString &revision,
                                   const QString &intervalId, const QString &subIntervalId, bool aggregate,
                                   qlonglong wellId, const QString &assetRef, const QString &status,
                                   const QVariantMap &detail = {} );

} // namespace paleo::strat
