// 层：数据
#pragma once

#include <qgsgeometry.h>

#include <QString>
#include <QVariantList>
#include <QVector>

#include <cstdint>
#include <functional>
#include <vector>

class QgsFeatureSource;

// 网格化引擎（距离变换 / 最小曲率）的约束读取：按统一语义
// （domain/constraintsemantics）逐条、逐部件判定，只把**完全阻断**打断线交给
// 引擎；停用、display_only/none 等非阻断模式、非法参数、无效几何一律不用并
// 留回执（不再只看 type 列）。
namespace paleo::algorithms
{

struct UnifiedBarrierRead
{
  QVector<QgsGeometry> parts; // 单部件折线，已过 transform
  QVariantList receipts;      // ConstraintUseDecision::toVariant 逐条/逐部件
  QVector<int> partReceipt;   // parts[i] 对应的 receipts 下标
};

// P3：把 parts 逐部件半格超采样栅格化进 mask（行主序，原点为左上角）。
// 一个格都没落上的部件不参与隔断：其回执改为 consumed=false、
// reason=outside_compute_domain（与曲面网格化同口径）。
// 返回落格部件数；cancelled() 为真时返回 -1（mask 内容作废）。
int rasterizeBarrierParts( UnifiedBarrierRead &read, double originX, double originYTop, double cellSize, int cols,
                           int rows, std::vector<std::uint8_t> &mask, const std::function<bool()> &cancelled );

// transform 对每个要素几何就地变换，失败须抛 QgsProcessingException。
UnifiedBarrierRead readUnifiedHardBarriers( QgsFeatureSource *source, const QString &methodId,
                                            const std::function<void( QgsGeometry & )> &transform );

// 回执 → 紧凑 JSON（Processing 字符串输出 CONSTRAINT_RECEIPTS）。
QString constraintReceiptsJson( const QVariantList &receipts );

} // namespace paleo::algorithms
