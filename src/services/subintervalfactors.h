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

// 自 DS bindings 拉编图证据行（按 kind 过滤）；稳定 ID，不匹配 horizon 名。
QVariantList evidenceRowsFromBindings( const std::vector<paleo::data::SubIntervalResultBinding> &bindings,
                                       const QStringList &kinds = {} );

} // namespace paleo::strat
