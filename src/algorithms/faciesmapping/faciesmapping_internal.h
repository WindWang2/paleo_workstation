// 层：数据
#pragma once

// 方向70（Unity 清障）：faciesmapping 的 candidateboundaries/faciesqa 匿名
// namespace 各带一份同构 geosArea（GEOSArea_r 包装），UNITY_BUILD 合批即
// 重定义。收拢单一定义；调用点零改动（同 cluster_internal.h 口径）。
// GeosContext 属 paleo::singlefactor（geosutil.h），签名按原实现全限定。

#include "../singlefactor/geosutil.h"

namespace paleo::faciesmapping {

inline double geosArea(const paleo::singlefactor::GeosContext &ctx, const GEOSGeometry *geometry)
{
  double area = 0;
  if (geometry)
    GEOSArea_r(ctx.handle, geometry, &area);
  return area;
}

} // namespace paleo::faciesmapping
