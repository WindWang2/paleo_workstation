// 层：数据
#pragma once

#include <functional>
#include <string>
#include <vector>

// algorithms/geostat — 地质统计学方法包的纯数值共享 DTO。
// 无 QtWidgets、无 QGIS 图层、无文件发布。克里金系全部消费这些值。
// 层：数据
namespace paleo::geostat
{

struct Point2
{
  double x = 0;
  double y = 0;
};

// 井/点样。独立于 singlefactor::Sample（免模块耦合），编排层负责转换；
// 变差函数/克里金/SGS 只关心坐标与值。
struct Sample
{
  double x = 0;
  double y = 0;
  double value = 0;
};

// 网格：无栅格类（singlefactor/gridsolver 同口径）。origin 为左上像元边，
// north-up 时 pixelHeight < 0，行向南，采样在像元中心；row-major，
// 行 0 = 最大 y。输出数组下标 row * cols + column，NaN = 缺失。
struct GridSpec
{
  int cols = 0;
  int rows = 0;
  double originX = 0;
  double originY = 0;
  double pixelWidth = 1;
  double pixelHeight = -1;
  std::string crs;

  bool isValid() const
  {
    return cols > 0 && rows > 0 && pixelWidth > 0 && pixelHeight < 0;
  }

  double cellCenterX( int column ) const { return originX + ( column + 0.5 ) * pixelWidth; }
  double cellCenterY( int row ) const { return originY + ( row + 0.5 ) * pixelHeight; }
};

struct Control
{
  std::function<bool()> cancelled;
  std::function<void( double )> progress; // [0,1]，调用方保证单调
};

enum class Status
{
  Ok,
  Cancelled,
  InvalidInput,
  NumericalFailure
};

inline const char *statusName( Status status )
{
  switch ( status )
  {
    case Status::Ok:
      return "ok";
    case Status::Cancelled:
      return "cancelled";
    case Status::InvalidInput:
      return "invalid_input";
    case Status::NumericalFailure:
      return "numerical_failure";
  }
  return "invalid_input";
}

} // namespace paleo::geostat
