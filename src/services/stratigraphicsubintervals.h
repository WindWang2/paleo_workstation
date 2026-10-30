// 层：数据
#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <vector>

#include "domain/stratigraphicunit.h"
#include "paleo_data/stratigraphic_framework.h"

namespace paleo::data { class DataService; }

// services/ — 四子层段体系的宿主适配层。Data Service 是唯一数据权威；
// 本层只做：由工程界面名单 + 井分层构造方案草稿、提交/读取、为
// 单井/地震/单因素/编图提供按 SubIntervalId 的取数与成果登记。
// 不持久化任何副本，不写死任何层位名。
namespace paleo::strat
{

// 井分层输入：wellId → (界面名 → 深度)。缺失的界面不出现。
using WellTopTable = QHash<qlonglong, QHash<QString, double>>;

// 以有序界面名单（浅→深，来自工程参数）构造方案草稿：相邻界面成父单元、
// 每个父单元 4 个等比初始子层段（provisional）；井出现由该井的上下界面
// 深度决定，上界缺失 → top_missing，下界缺失 → not_penetrated（不补造深度）。
paleo::data::StratigraphicScheme draftScheme( const QString &schemeId, const QString &name,
                                              const QStringList &orderedBoundaries, const WellTopTable &tops,
                                              const QString &depthType = QStringLiteral( "MD" ) );

// 二级选择器选项：父单元 → S1–S4。
struct SelectorParent
{
  QString intervalId;
  QString label; // "T70–T71"
  QStringList subIntervalIds; // 恰好 4 个，S1..S4
  QStringList subLabels;      // "S1".."S4"
};
QVector<SelectorParent> selectorOptions( const paleo::data::StratigraphicScheme &scheme );

// 由选择器状态构造任务选择；subIndex=0 表示父单元显式综合。
UnitSelection makeSelection( const paleo::data::StratigraphicScheme &scheme, const QString &intervalId, int subIndex );

// 单井相/测井样点按子层段过滤：返回与 depths 等长的掩码（1=属于该子层段）。
// 区间半开 [top,bottom)，最后一个子层段含底界；井无该子层段 → 全 0 且 reason 非空。
QVector<char> wellSampleMask( const paleo::data::StratigraphicScheme &scheme, qlonglong wellId,
                              const QString &subIntervalId, const QVector<double> &depths, QString *reason = nullptr );

// 单因素井点取值：只在该子层段内汇总（均值）；样点不足 → NaN，不回退到父单元。
double wellSubIntervalMean( const paleo::data::StratigraphicScheme &scheme, qlonglong wellId,
                            const QString &subIntervalId, const QVector<double> &depths,
                            const QVector<double> &values, int minSamples = 1 );

// 父单元显式综合（厚度加权）：任一子层段无值 → NaN 且 missing 列出缺失的 Sn。
double aggregateInterval( const paleo::data::StratigraphicScheme &scheme, qlonglong wellId, const QString &intervalId,
                          const QHash<QString, double> &perSubValue, QStringList *missing = nullptr );

// 父单元下各子层段的井出现 + 成果绑定就绪摘要（展开状态行 / 红绿灯）。
// wellOccurrences 挂在 IntervalId；同一父单元四子层段共享井出现计数，
// 再按 SubIntervalId 叠加 bindings（可用资料 / 受影响）。
struct SubIntervalOccurrenceSummary
{
  QString subIntervalId;
  QString label; // S1..S4
  QString light; // green|yellow|red|gray
  QString line;  // 一行中文（含灯号）
  int wellsComplete = 0;
  int wellsAffected = 0; // top_missing|not_penetrated|pinched_out|faulted
  int wellsPending = 0;
  int bindingsCurrent = 0;
  int bindingsStale = 0; // stale|review_required
};

QVector<SubIntervalOccurrenceSummary> summarizeSubIntervalOccurrences(
    const paleo::data::StratigraphicScheme &scheme, const QString &intervalId,
    const std::vector<paleo::data::SubIntervalResultBinding> *bindings = nullptr );

} // namespace paleo::strat
