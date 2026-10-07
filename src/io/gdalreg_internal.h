// 层：数据
#pragma once

// 方向70（Unity 清障）：constraintstore/perffixtures/rasterpyramid 匿名
// namespace 各带一份同构 ensureGdalRegistered（call_once + GDALAllRegister），
// UNITY_BUILD 合批即重定义。收拢单一定义；各 .cpp 匿名 namespace 顶部
// `using paleo::io_detail::ensureGdalRegistered;` 接线，调用点零改动。

#include <gdal.h>

#include <mutex>

namespace paleo::io_detail {

/// 进程内幂等注册 GDAL 驱动（多 TU 共享同一次 call_once）。
inline void ensureGdalRegistered()
{
  static std::once_flag flag;
  std::call_once(flag, []() { GDALAllRegister(); });
}

} // namespace paleo::io_detail
