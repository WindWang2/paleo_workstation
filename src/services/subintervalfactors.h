// 层：数据
#pragma once
#include "domain/subintervalfacies.h"
#include "domain/stratigraphicunit.h"
#include "domain/wellsection.h"
#include "paleo_data/stratigraphic_framework.h"

#include <QString>
#include <QVariantMap>
#include <vector>
#include <QVector>
#include <optional>

namespace paleo::data
{
class DataService;
}
namespace paleo::domain
{
class LithoLexicon;
}

// services/ — 子层段砂地比/井相/地震相成果：取数、分类、写入 Data Service、编图证据行。
namespace paleo::strat
{

// 稳定井键：UTF-8 FNV-1a 64 位（跨运行稳定；供 DS wellId 与证据行对齐）。
qlonglong stableWellKey( const QString &entityId );

// 自井段岩性在 [top,bottom] 内累计砂厚（m）。无有效段 → nullopt。
// sandIn：优先 lexicon.isSand(litho)，否则名称含「砂」/Sand。
std::optional<double> sandThicknessInRange( const QVector<wellsection::LithoSegment> &segments, double topMd,
                                            double bottomMd, const paleo::domain::LithoLexicon *lexicon );

// 在已提交方案上解析井 × 子层段深度窗并合成砂地比；失败时 metrics.ok=false。
SandRatioMetrics sandRatioForWellSubInterval( const paleo::data::StratigraphicScheme &scheme,
                                              const QString &subIntervalId, qlonglong wellId,
                                              const QVector<wellsection::LithoSegment> &segments,
                                              const paleo::domain::LithoLexicon *lexicon = nullptr,
                                              const SandRatioBins &bins = {} );

// 无已提交方案时：由父单元顶底 + 子层段序号（1..4）等比窗合成砂地比（provisional）。
SandRatioMetrics sandRatioForProvisionalSub( double parentTopMd, double parentBottomMd, int subIndex1to4,
                                             const QVector<wellsection::LithoSegment> &segments,
                                             const paleo::domain::LithoLexicon *lexicon = nullptr,
                                             const SandRatioBins &bins = {} );

struct BindResult
{
  bool ok = false;
  QString error;
  paleo::data::SubIntervalResultBinding stored;
  QVariantMap evidenceRow;
};

// 将砂地比单因素绑定写入 Data Service（kind=single_factor）。
BindResult bindSandRatioFactor( paleo::data::DataService &ds, const UnitSelection &sel, qlonglong wellId,
                                const SandRatioMetrics &metrics, const QString &assetRef = {} );

// 将井相分类绑定写入 DS（kind=well_facies）；detail 含砂地比与分类码。
BindResult bindWellFaciesFromSandRatio( paleo::data::DataService &ds, const UnitSelection &sel, qlonglong wellId,
                                        const SandRatioMetrics &metrics, const QString &assetRef = {} );

// 地震相绑定：scope=slice|interval；interval 必须带 coverage/strategy，禁止切片冒充整段。
BindResult bindSeismicFaciesResult( paleo::data::DataService &ds, const UnitSelection &sel,
                                    SeismicFaciesScope scope, int classCode, double coverage,
                                    const QString &strategyId, const QString &assetRef,
                                    const QString &status = QStringLiteral( "current" ) );

// 已知砂地比 + 深度窗 → 指标（砂厚 = ratio × 层厚）。
SandRatioMetrics metricsFromKnownRatio( double topMd, double bottomMd, double sandRatio,
                                        const SandRatioBins &bins = {} );

struct AutoBindReport
{
  bool attempted = false;
  int boundWells = 0;
  int skipped = 0;
  QString message;
};

// 单因素提取收口：当 UnitSelection 有效且工程有 area-model 时，对 contributing
// 井行自动 bindSandRatioFactor + bindWellFaciesFromSandRatio。
// 深度窗优先行内 windowTopMd/windowBottomMd；否则用 parentTop/Bottom+subIndex 等比窗。
// DS 不可用或选择无效 → attempted=false，不抛错（提取本身已成功）。
AutoBindReport autoBindSandRatioExtraction( const QString &projectDir, const UnitSelection &sel,
                                            const QVariantList &rows, const QString &assetRef,
                                            double parentTopMd = 0, double parentBottomMd = 0 );

// 切片投票 → 层段综合并写入 DS（先可写若干 slice 绑定，再写 interval）。
// Slice 与 Interval 分列；Interval 强制 weighted_mode + coverage。
struct SeismicPublishReport
{
  bool ok = false;
  QString error;
  BindResult intervalBind;
  QVector<BindResult> sliceBinds;
  SeismicIntervalAggregate aggregate;
};

SeismicPublishReport publishSeismicSliceAggregate( paleo::data::DataService &ds, const UnitSelection &sel,
                                                   const QVector<SeismicSliceVote> &votes,
                                                   const QString &intervalAssetRef,
                                                   double minCoverage = 0.5,
                                                   bool alsoBindSlices = false,
                                                   const QString &sliceAssetPrefix = {} );

// 打开工程 area-model 的 Data Service（供视图层绑定，避免 ui→io）。
// 无 data.sqlite / 打开失败 → false，error 可读中文原因。
bool openProjectDataService( const QString &projectDir, paleo::data::DataService &ds, QString *error = nullptr );

// 自 DS bindings 拉编图证据行（按 kind 过滤）；稳定 ID，不匹配 horizon 名。
QVariantList evidenceRowsFromBindings( const std::vector<paleo::data::SubIntervalResultBinding> &bindings,
                                       const QStringList &kinds = {} );

} // namespace paleo::strat
