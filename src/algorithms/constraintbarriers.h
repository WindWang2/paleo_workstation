// 层：数据
#pragma once

#include <qgsgeometry.h>

#include <QPointF>
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

// P1（pr457-closure）：把 parts 逐部件栅格化进 mask（行主序，原点为左上角）。
// 每段先按闭矩形裁剪到计算域（与计划侧 polylineTouchesRect 同口径，贴边点
// clamp 进边缘格），再走 supercover 格网遍历——45°/陡斜线/穿角点不留对角
// 漏格，屏障距离场不可对角穿透。一个格都没落上的部件不参与隔断：其回执改
// 为 consumed=false、reason=outside_compute_domain。落格部件的回执细化：
// rasterized=true + rasterized_cells（唯一格数）+ new_cells（本部件新置的
// 格数）；new_cells=0 表示完全被既有屏障遮蔽（warnings 加
// fully_shadowed_by_other_barriers）。返回落格部件数；cancelled() 为真时
// 返回 -1（mask 内容作废）。
int rasterizeBarrierParts( UnifiedBarrierRead &read, double originX, double originYTop, double cellSize, int cols,
                           int rows, std::vector<std::uint8_t> &mask, const std::function<bool()> &cancelled );

// transform 对每个要素几何就地变换，失败须抛 QgsProcessingException。
UnifiedBarrierRead readUnifiedHardBarriers( QgsFeatureSource *source, const QString &methodId,
                                            const std::function<void( QgsGeometry & )> &transform );

// 单条折线（多部件已拆）的 supercover 格网遍历（P1，pr457-closure）：每段先按
// 闭矩形裁剪到计算域（贴右/下边界的点 clamp 进最后一列/行格），再整数误差项
// 遍历，对每个途经格调用 visit(col,row)。穿格点先跨竖线再跨横线，途经格链保持
// 4-连通（距离场不可对角穿透）；只触角点的另一侧格保持自由。pixelHeight 为负
// （原点在左上、向下）。cols/rows 非正直接返回。这是各栅格化点（距离变换/
// 最小曲率/旧 IDW 网格连通/本地方向屏障）的单一空间判定实现——不再各自半格
// 采样（陡斜线/穿角点漏格）。
void supercoverPolylineCells( const QVector<QPointF> &vertices, double originX, double originYTop,
                              double pixelWidth, double pixelHeight, int cols, int rows,
                              const std::function<void( int, int )> &visit );

// 回执 → 紧凑 JSON（Processing 字符串输出 CONSTRAINT_RECEIPTS）。
QString constraintReceiptsJson( const QVariantList &receipts );

} // namespace paleo::algorithms
