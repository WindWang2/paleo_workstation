// 层：数据
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// 方向41：参考工程可枚举历史制图策略的参数包词表（不做 GUI 全复刻）。
//
// 词表来源（只读参考，固定 SHA 27fdb998a32d7a7f50d7e6ef0d2ebb3a5d06378f）：
//   Drawing/drawing/single_factor/methods/registry.py          —— 曲面方法表
//   Drawing/drawing/single_factor/contour_methods/registry.py  —— 提线方法表
//   Drawing/drawing/single_factor/workflow.py                  —— 工作场版本/几何策略
//
// 每条参数包都显式声明：映射到本仓哪个 Processing 引擎、哪些参数本仓尚未实现
// （notImplemented 逐条列出，不静默忽略）。未知 id 一律返回 nullptr——不默认
// 回落到某个「差不多」的策略，标签与真实算法必须一致。
// 层：数据
namespace paleo::singlefactor
{

struct SurfaceMethodPack
{
  QString id;      // 上游 method_id（UI 与参数持久化都用它）
  QString label;   // 上游标签原文（中文）
  QString category;
  // 本仓算法 id：既是有独立 Processing 入口的引擎名（paleo:paleo_*），
  // 也是写进血缘/QC 的 algorithm_id。`local_direction_kriging` 与 IDW 共用
  // 同一个 Processing 入口（paleo_local_direction_idw + METHOD=kriging），
  // 但血缘用独立 id 区分真实引擎——回落时血缘写的是 IDW 的 id，不冒充。
  QString engineId; // 空 = 未实现
  bool supportsConstraints = false;
  bool requiresScipy = false;
  bool implemented = false;
  QString geologicalNote;
};

// 上游 SINGLE_FACTOR_METHODS 的四条 + 本仓克里金接入后新增的局部方向克里金。
const QVector<SurfaceMethodPack> &surfaceMethodPacks();
// 未登记 id → nullptr（调用方必须报错，不得回退）。
const SurfaceMethodPack *surfaceMethodPack( const QString &id );

struct ContourExtractPack
{
  QString id;
  QString label;
  QString pipelineNote;
  QString geologicalNote;
  bool supportsPartition = true;
  bool supportsBarrierStop = true;
  bool clipToSurface = true;
  int upsampleFactor = 1;
  int smoothingIterations = 0;
  double simplifyToleranceRatio = 0;   // × gridStep
  double bridgeGapRatio = 0;           // × gridStep
  double minContourLengthRatio = 0;    // × gridStep
  // 本仓消费的参数名（与 cartographicsmooth.h / fieldcontours.h 对齐）；
  // 不在表里的上游参数仍未实现，不假装消费。
  QStringList consumedParameters;
  QStringList notImplemented;
};

const QVector<ContourExtractPack> &contourExtractPacks();
const ContourExtractPack *contourExtractPack( const QString &id );

// 参数包 → 绝对参数（gridStep 单位与工程距离一致）。
struct ContourExtractParameters
{
  QString methodId;
  int upsampleFactor = 1;
  int smoothingIterations = 0;
  double simplifyTolerance = 0;
  double bridgeGap = 0;
  double minContourLength = 0;
  bool clipToSurface = true;
};

ContourExtractParameters resolveContourExtract( const ContourExtractPack &pack, double gridStep );

struct CartographicWorkPack
{
  QString id;
  QString label;
  QString partitionVersion; // 上游 contour_partition.version
  QString geometryPolicy;   // 上游 geometry_policy
  double barrierBufferDistance = 0;
  double contourStopBufferDistance = 0;
  bool requiresAnalysisField = true;
  bool implemented = false;
  QString note;
};

const QVector<CartographicWorkPack> &cartographicWorkPacks();
const CartographicWorkPack *cartographicWorkPack( const QString &id );

} // namespace paleo::singlefactor
