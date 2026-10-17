// 层：数据
#pragma once

// 方向70（Unity 清障）：cluster/som/supervised 匿名 namespace 各带一份同构
// stopped/report（取消轮询 + 进度转发），UNITY_BUILD 合批即重定义。收拢为
// 单一定义；各 .cpp 去本地定义后 unqualified 调用经 paleo::cluster 命名空间
// 命中，调用点零改动。localidw 的同名 helper 在 paleo::singlefactor 且
// report 带钳制（语义不同），不并入。

#include "cluster.h"

#include <cstddef>
#include <random>
#include <utility>

namespace paleo::cluster {

/// 取消轮询：Control 带回调才视为取消。
inline bool stopped( const Control &c ) { return c.cancelled && c.cancelled(); }

/// 进度转发（无回调静默丢弃）。
inline void report( const Control &c, double p )
{
  if ( c.progress )
    c.progress(p);
}

/// #470：cluster 域确定随机口径——std::uniform_int/real_distribution 与
/// std::shuffle 的输出序列是标准库实现定义（libstdc++/libc++/MSVC 互不
/// 相同），违背项目「同 seed 跨平台逐位一致」口径（geostat/sgs.h 已明文
/// 禁用 std::normal_distribution 的同款理由）。全部抽样改走 mt19937_64
/// 原始 u64 的自订推导，与 sgs 内核同源：
///   * unitRandom：u64 → 53bit 尾数 (0,1) 单值（geostat/sgs_internal.h 同款）；
///   * randomBelow：单次 u64 取模 [0,bound)（sgs.cpp Fisher–Yates 的
///     j = rng() % i 同款；bound << 2^64 模偏置可忽略）；
///   * shuffleDeterministic：手写 Fisher–Yates（sgs.cpp 随机路径同构）。
/// 分布语义与被替换的 std 分布一致（同分布族，非逐位同序列）。

/// 确定性 (0,1) 均匀：mt19937_64 原始 u64 → 53bit 尾数。
inline double unitRandom( std::mt19937_64 &rng )
{
  return ( static_cast<double>( rng() >> 11 ) + 0.5 ) * 0x1.0p-53;
}

/// 确定性 [0, bound) 均匀整数（单次原始 u64 取模）。
inline std::size_t randomBelow( std::mt19937_64 &rng, std::size_t bound )
{
  return bound > 1 ? static_cast<std::size_t>( rng() % bound ) : std::size_t( 0 );
}

/// 确定性洗牌：Fisher–Yates（随机访问迭代器；i 从高到低，j = rng() % i）。
template <typename Iterator>
inline void shuffleDeterministic( Iterator first, Iterator last, std::mt19937_64 &rng )
{
  const std::size_t n = static_cast<std::size_t>( last - first );
  for ( std::size_t i = n; i > 1; --i )
  {
    const std::size_t j = static_cast<std::size_t>( rng() % i );
    std::swap( first[i - 1], first[j] );
  }
}

} // namespace paleo::cluster
