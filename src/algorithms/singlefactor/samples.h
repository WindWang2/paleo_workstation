// 层：数据
#pragma once

#include "types.h"

#include <optional>
#include <string>

// 层：数据
namespace paleo::singlefactor
{

enum class CrsMode
{
  Projected,
  LocalEngineering,
  Missing,
  Mixed
};

struct SamplePrepRequest
{
  std::vector<Sample> rows;
  CrsMode crs = CrsMode::Projected;
  std::string valueUnit;
  // 两端都设置时才启用范围过滤。0 可以是有效值，不会被当成缺失。
  std::optional<double> minimum;
  std::optional<double> maximum;
  // 显式百分数转比例。未设置时保留原值，不截到 [0,1]。
  bool percentToFraction = false;
};

struct SamplePrepResult
{
  Status status = Status::InvalidInput;
  std::string message;
  PreparedInput input;
};

SamplePrepResult prepareSamples( const SamplePrepRequest &request );

} // namespace paleo::singlefactor
