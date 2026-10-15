// 层：数据
#pragma once

#include <qgsgeometry.h>

#include <QString>
#include <QVariantList>
#include <QVector>

#include <functional>

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
};

// transform 对每个要素几何就地变换，失败须抛 QgsProcessingException。
UnifiedBarrierRead readUnifiedHardBarriers( QgsFeatureSource *source, const QString &methodId,
                                            const std::function<void( QgsGeometry & )> &transform );

// 回执 → 紧凑 JSON（Processing 字符串输出 CONSTRAINT_RECEIPTS）。
QString constraintReceiptsJson( const QVariantList &receipts );

} // namespace paleo::algorithms
