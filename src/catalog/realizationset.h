// 层：数据
#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVector>

class DataCatalog;
struct CatalogVersion;

// catalog/realizationset —— 多 realization（不确定性集合）契约与查询面。
// （方向 47；TODOS P2「多 realization / 不确定性支持」落地）
//
// 契约钉死（改这里 = 改 tests/tst_realizationset.cpp）：
//
//   · 集合 = 一个 catalog 资产，type == "realization_set"；成员 = 该资产名下
//     的 DERIVED 版本，每版本 extra 携带：
//       realizationSetId        集合锚（== 所属资产 id；成员行自描述用）
//       realizationIndex        int，0 起；成员序的唯一依据
//       realizationMemberCount  生成时声明的成员数（缺号检测的分母）
//       realizationSeed         u64——SGS 共享随机流的基种子（sgs.h 契约：
//                               各实现共享同一条随机流，成员间靠 index 区分
//                               取样序，不是每成员独立种子）
//   · 统计面 = 独立资产 type "realization_stat" 的 DERIVED 版本，extra 带：
//       realizationSetId / realizationStatistic（token，见下）/
//       realizationMemberCount（参与统计的成员数）
//     parentVersionIds = 统计时全部在场成员版本 id（锚全体——provenance
//     可复算；成员缺号时统计面如实锚「在场成员」，缺的成员不进 parents）。
//   · 集合差值面 = 资产 type "realization_diff" 的 DERIVED 版本：
//     parentVersionIds = 两侧均值面版本 id；extra 带 realizationSetIds（两元
//     QStringList）+ realizationStatistic="mean_diff"。
//
// 「realization_id 预留」的落地形态（PALEO_QGIS_PLAN D6 注记）：物化为
// versions.extra_json 内的键族，而不是 sqlite 新列——
//   · extra_json 本就整体 round-trip（catalogstore.cpp），无独立列可空语义
//     可借；且成员需要 index/seed/count 多键，单列装不下；
//   · catalog_meta.schema_epoch 维持 1，无迁移：旧构建读新 catalog 只是
//     不解释键（extra 原样回写不丢），新构建读旧 catalog 缺键即非成员——
//     前后向兼容都靠「缺键 = 非集合」语义成立。SCHEMA_MIGRATION.md §8 注记。
//
// 正交关系决议：成员是「同一版本语义下的等概率兄弟」，不是版本序列——
// versionNumber 在集合资产内只做分配序（= realizationIndex+1，由
// DerivedAssetRegistrar 递增分配），不带「新旧」语义。currentVersion()
// 对集合资产返回最高号成员是定义内行为但语义上是任意成员；集合消费方
// 一律走本面的成员寻址，勿用 currentVersion() 读集合。
//
// 诚实口径（UI/文案不得越称）：
//   · 集合不全（缺成员号）如实列缺号集，不静默按完整算；
//   · N=1 集合合法存在但无不确定性——统计派生拒绝，UI 标「单成员」；
//   · stddev 是总体口径（÷n，与 constraintfactorjobs SGS 旁路一致），
//     不与样本口径 ÷(n-1) 混称；
//   · realization 集合是等概率实现族，不是概率校准——文案不出现
//     「真实概率」「置信区间概率」。

namespace paleo::realization
{

// ---- extra 键词表（写侧 workflow / 读侧 UI 同源，勿另起字面量）-------------
inline const QString kAssetTypeSet = QStringLiteral( "realization_set" );
inline const QString kAssetTypeStat = QStringLiteral( "realization_stat" );
inline const QString kAssetTypeDiff = QStringLiteral( "realization_diff" );
inline const QString kKeySetId = QStringLiteral( "realizationSetId" );
inline const QString kKeyIndex = QStringLiteral( "realizationIndex" );
inline const QString kKeyMemberCount = QStringLiteral( "realizationMemberCount" );
inline const QString kKeySeed = QStringLiteral( "realizationSeed" );
inline const QString kKeyStatistic = QStringLiteral( "realizationStatistic" );
inline const QString kKeySetIds = QStringLiteral( "realizationSetIds" ); // diff（两元）

// ---- 统计口径 token（落盘值，稳定不本地化）---------------------------------
inline const QString kStatMean = QStringLiteral( "mean" );
inline const QString kStatStdDev = QStringLiteral( "stddev_population" );
inline const QString kStatP10 = QStringLiteral( "p10" );
inline const QString kStatP90 = QStringLiteral( "p90" );
inline const QString kStatMeanDiff = QStringLiteral( "mean_diff" );

// token → 展示口径词（QCoreApplication::translate——「成员均值」「成员总体
// 标准差（÷N)」「成员 P10」「成员 P90」「两集合均值差」。图签/版本面板/树
// 节点共用同一映射，保证口径逐字一致）。未知 token → 空串（调用方如实降级）。
QString statisticDisplayLabel( const QString &token );

// ---- 查询 DTO -------------------------------------------------------------
struct RealizationMember
{
  QString versionId;
  int index = -1;        // extra["realizationIndex"]；缺键/坏值不会进集合
  quint64 seed = 0;      // extra["realizationSeed"]（缺省 0）
  QString fileName;
};

struct RealizationSet
{
  QString setId;      // 集合资产 id；空 = 未找到
  QString assetId;    // == setId（集合即资产；字段留名给读侧表达）
  QString title;      // 资产 displayName
  int declaredCount = 0;              // 成员 extra 声明的最大 memberCount
  QVector<RealizationMember> members; // 按 index 升序
  QVector<int> missingIndices;        // [0, declaredCount) 中缺席的 index
  // 完整 = 声明数与实际成员数一致且无缺号。declaredCount==0（成员全缺或
  // 从未写计数键）不算完整——缺号无从判定时不冒充完整。
  bool complete() const
  {
    return declaredCount > 0 && missingIndices.isEmpty() &&
           members.size() == declaredCount;
  }
  bool isEmpty() const { return members.isEmpty(); }
};

struct StatisticSurface
{
  QString versionId;
  QString token;         // kStatMean / kStatStdDev / kStatP10 / kStatP90
  int memberCount = 0;   // 统计时在场成员数
  QString assetId;
};

// 枚举库内全部集合（versions 全表扫一次分组；成员识别 = 版本 extra 同带
// realizationSetId + 合法 realizationIndex）。catalog 未开/无集合 → 空表。
QVector<RealizationSet> enumerateSets( const DataCatalog &catalog );

// 单集合寻址：setId 即集合资产 id。不存在/无成员 → setId 空的返回值。
RealizationSet setById( const DataCatalog &catalog, const QString &setId );

// 成员寻址：index → 成员版本 id。缺号或未知 index → 空串。
QString memberVersionId( const RealizationSet &set, int index );

// 集合的全部统计面版本（realization_stat 资产下 extra.realizationSetId 匹配
// 的 DERIVED 版本），按 token 稳定排序（mean, stddev, p10, p90 词表序后接
// 未知 token 字典序）。
QVector<StatisticSurface> statSurfaces( const DataCatalog &catalog,
                                        const QString &setId );

// 集合均值面版本 id（token==mean 的最新一条；缺 → 空串）。
QString meanSurfaceVersionId( const DataCatalog &catalog, const QString &setId );

// 动画帧序列：成员栅格的受管绝对路径按 index 升序。读不到受管路径的成员
// 跳过（路径安全性由 DataCatalog::resolvedVersionPath 保证）。
QStringList memberRasterPaths( const DataCatalog &catalog, const QString &projectDir,
                               const QString &setId );

} // namespace paleo::realization
