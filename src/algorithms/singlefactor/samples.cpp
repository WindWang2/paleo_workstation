// 层：数据
#include "samples.h"

#include <cmath>
#include <map>
#include <utility>

// 层：数据
namespace paleo::singlefactor
{

SamplePrepResult prepareSamples( const SamplePrepRequest &request )
{
  SamplePrepResult result;
  PreparedInput &input = result.input;
  input.originalCount = static_cast<int>( request.rows.size() );
  input.valueUnit = request.valueUnit;
  input.localEngineeringGrid = request.crs == CrsMode::LocalEngineering;
  if ( request.crs == CrsMode::Missing )
  {
    result.message = "输入没有 CRS，且未显式使用局部工程坐标";
    return result;
  }
  if ( request.crs == CrsMode::Mixed )
  {
    result.message = "已知 CRS 与未知 CRS 不能混用";
    return result;
  }
  const bool useRange = request.minimum.has_value() && request.maximum.has_value();
  if ( useRange && !( *request.minimum < *request.maximum ) )
  {
    result.message = "数值范围下限必须小于上限";
    return result;
  }
  if ( request.percentToFraction )
    input.valueUnit = request.valueUnit == "%" || request.valueUnit.empty() ? "fraction" : request.valueUnit;

  struct Group
  {
    int count = 0;
    double first = 0;
    bool conflict = false;
  };
  std::map<std::pair<double, double>, Group> groups;
  for ( const Sample &row : request.rows )
  {
    if ( !std::isfinite( row.value ) || !std::isfinite( row.x ) || !std::isfinite( row.y ) )
    {
      ++input.missingCount;
      continue;
    }
    double value = row.value;
    if ( useRange && ( value < *request.minimum || value > *request.maximum ) )
    {
      ++input.outOfRangeCount;
      continue;
    }
    if ( request.percentToFraction )
      value *= 0.01;
    Sample kept = row;
    kept.value = value;
    input.samples.push_back( std::move( kept ) );
    Group &group = groups[{ row.x, row.y }];
    if ( group.count == 0 )
      group.first = value;
    else if ( group.first != value )
      group.conflict = true;
    ++group.count;
  }
  input.validCount = static_cast<int>( input.samples.size() );
  for ( const auto &entry : groups )
  {
    if ( entry.second.count > 1 )
      input.duplicateExtraRows += entry.second.count - 1;
    if ( entry.second.conflict )
      input.conflictRows += entry.second.count;
  }
  if ( input.validCount == 0 )
  {
    result.message = "没有可用的有限井点数值";
    return result;
  }
  if ( input.originalCount != input.validCount + input.missingCount + input.outOfRangeCount )
  {
    result.status = Status::NumericalFailure;
    result.message = "样本计数不平衡";
    return result;
  }
  result.status = Status::Ok;
  return result;
}

} // namespace paleo::singlefactor
