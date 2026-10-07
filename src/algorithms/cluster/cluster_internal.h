// 层：数据
#pragma once

// 方向70（Unity 清障）：cluster/som/supervised 匿名 namespace 各带一份同构
// stopped/report（取消轮询 + 进度转发），UNITY_BUILD 合批即重定义。收拢为
// 单一定义；各 .cpp 去本地定义后 unqualified 调用经 paleo::cluster 命名空间
// 命中，调用点零改动。localidw 的同名 helper 在 paleo::singlefactor 且
// report 带钳制（语义不同），不并入。

#include "cluster.h"

namespace paleo::cluster {

/// 取消轮询：Control 带回调才视为取消。
inline bool stopped(const Control &c) { return c.cancelled && c.cancelled(); }

/// 进度转发（无回调静默丢弃）。
inline void report(const Control &c, double p)
{
  if (c.progress)
    c.progress(p);
}

} // namespace paleo::cluster
