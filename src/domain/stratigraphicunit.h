// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <QPair>
#include <QVector>

// domain/ — 宿主侧「四子层段」选择与历史标签分类（docs/STRATIGRAPHIC_SUBINTERVALS.md）。
// 权威模型在 Data Service（paleo_data/stratigraphic_framework.h）；本文件只放
// 视图/功能层都要用的纯值类型，不依赖 Data Service、不持久化。
//
// 核心语义：
//   · 地震层位/界面（如 mappingHorizons() 的成员）是 Boundary，不是地层单元；
//   · 相邻两条界面构成父单元（Interval，显示为「上界–下界」）；
//   · 每个父单元固定 4 个子层段 S1–S4（自上而下）；
//   · 新的分析/编图/结果只按 SubIntervalId 归属；父单元综合必须显式 aggregate。
namespace paleo::strat
{

inline constexpr int kSubIntervalCount = 4;

// 一次任务的层段选择：方案 + 修订 + 父单元 + 子层段。
struct UnitSelection
{
  QString schemeId;
  QString revision;      // 空 = 当前头修订（执行前必须解析为具体修订再写入任务记录）
  QString intervalId;
  QString subIntervalId; // aggregate=false 时必填
  bool aggregate = false; // true = 父单元显式派生综合（不是任何单一子层段）
  QString displayName;   // 仅用于显示，不参与身份

  bool isValid() const
  {
    if ( schemeId.isEmpty() || intervalId.isEmpty() )
      return false;
    return aggregate ? subIntervalId.isEmpty() : !subIntervalId.isEmpty();
  }
  bool operator==( const UnitSelection &o ) const
  {
    return schemeId == o.schemeId && revision == o.revision && intervalId == o.intervalId &&
           subIntervalId == o.subIntervalId && aggregate == o.aggregate;
  }
};

// 结果缓存键：方案 + 修订 + 单元身份 + 子层段指纹 + 参数摘要。显示名不进键。
QString cacheKey( const UnitSelection &selection, const QString &unitFingerprint, const QString &parameterSha256 );

// 图层/资产命名用的稳定令牌（如 "sub-xxxx" / "agg-int-xxxx"）；无效选择回空。
QString layerToken( const UnitSelection &selection );

enum class LabelKind
{
  Boundary,      // 单一界面名（T70）——不是单元，不能直接承载成果
  Interval,      // 相邻界面对（T70–T71）
  SubInterval,   // 子层段（T70–T71/S2）
  NonAdjacent,   // 跨多个父单元的旧段（如 T70–T72），不能直接改名为单元
  Unknown
};

struct LabelClassification
{
  LabelKind kind = LabelKind::Unknown;
  QString top;
  QString bottom;
  int subIndex = 0; // 1..4，仅 SubInterval
};

// 按有序界面名单（浅→深）分类一个历史标签。不按名字猜测：只有完整匹配
// 「界面」「相邻界面对」「相邻界面对/Sn」才给出对应类别。
LabelClassification classifyLabel( const QString &label, const QStringList &orderedBoundaries );

// 需要子层段归属的成果类型：新任务没有有效 SubIntervalId 时必须拒绝。
bool requiresSubInterval( const QString &resultKind );

// 有序界面 → 父单元（相邻对）。界面名单来自工程参数，不写死任何层位。
QVector<QPair<QString, QString>> adjacentIntervals( const QStringList &orderedBoundaries );

} // namespace paleo::strat
