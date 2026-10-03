// 层：数据
#pragma once

#include "partition.h"
#include "types.h"

#include <vector>

// regional_contours 移植（haiyou-visualization 27fdb99）。
// 分区独立井控等值线：每个解释分区用本区井点独立插值、独立提线，
// 不跨隔断借用邻区数值。分区内无井即拒绝（与上游一致，不静默外借）。
// 层：数据
namespace paleo::singlefactor
{

struct RegionalContourResult
{
  Status status = Status::InvalidInput;
  std::string message;
  std::vector<ContourPolyline> contours; // 各区等值线拼接
  std::vector<int> regionOfContour; // 与 contours 对齐的分区编号
  std::vector<std::string> issues;
};

// partition.wellRegionIds 必须与 input.samples 同序对齐。
RegionalContourResult regionalContours( const PreparedInput &input, const GridSpec &grid,
                                        const ResolvedParameters &parameters,
                                        const std::vector<double> &levels,
                                        const PartitionResult &partition,
                                        const Control *control = nullptr );

} // namespace paleo::singlefactor
