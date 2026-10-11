// 层：数据
#pragma once
// 相栅格小图斑吸收（§36 faciespolygonize 的 sliver 步）。
//
// 语义（与旧实现逐位等价，见 tst_faciessliver 随机网格对拍）：每轮在全部
// 「可吸收」4 连通斑中选面积最小者（等面积取扫描序最靠前者），并入其最大
// 相邻相斑（等大取较小编码）；无相邻相斑的相斑清为 nodata。可吸收 =
// 像元数 < minCells 或 面积 < minArea；贴边 nodata（图外）不动，无相邻相斑
// 的 nodata 斑不动。
//
// 旧实现每并一斑全图重标号（O(K·N)，审查 A11：400² 约 12 s、不可取消）；
// 这里一次标号 + 并查集 + 惰性小顶堆，O(N + K·deg·log K)，并每若干次合并
// 轮询一次 cancelled。
#include <QVector>
#include <functional>

namespace paleo::facies
{
constexpr int kSliverNodata = -9999;

// 返回合并次数；cancelled() 返回 true 时中止并返回 -1（grid 处于半成品
// 状态，调用方应丢弃）。
int absorbSmallParts( QVector<int> &grid, int cols, int rows, double cellArea,
                      double minArea, int minCells,
                      const std::function<bool()> &cancelled = {} );
} // namespace paleo::facies
