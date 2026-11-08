// 层：数据
#pragma once

// singlefactor/sgsbarrier — P3：屏障分区核 → SGS 隔断输入的纯数据适配。
// labelHardBarriers / labelInterpretationPartition 产出的 BarrierGrid 在
// singlefactor 语境（格分量 + 井归属 + 引擎 consumedIds）；SGS 数值核
//（geostat/sgs.h SgsBarrierInput）只认纯值分量归属。这里做唯一一次转换，
// workflow 物化与算法层测试共用，两侧不各自手抄分量语义。
//
// 稳定重编号：分量号按「该分量在 row-major 扫描下首次出现的格索引」升序
// 重编——与泛洪起播序一致，但显式规范化后与约束行的存储顺序、容器遍历序
// 完全解耦（同 seed 同几何 → 同分量号 → 同随机流派生）。

#include "../geostat/sgs.h"
#include "types.h"

#include <vector>

namespace paleo::singlefactor
{

// barrierGrid：分区核输出（error 字段由调用方先检查）。
// samples：与传给分区核的同一井样本向量（sampleComponent 与其对齐）。
// barrierModel：分区模型名（如 "grid_connectivity_v1"），透传进 QC。
paleo::geostat::SgsBarrierInput toSgsBarrierInput( const BarrierGrid &barrierGrid,
                                                   const std::vector<Sample> &samples,
                                                   const std::string &barrierModel );

// 地统计算网格 → 分区核网格（字段同构，模块各自持有 GridSpec 防耦合；
// convention 按分区核既有约定固定 north_up_pixel_is_area）。
GridSpec toSinglefactorGrid( const paleo::geostat::GridSpec &grid );

} // namespace paleo::singlefactor
