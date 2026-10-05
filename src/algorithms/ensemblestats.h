// 层：数据
#pragma once

#include <cstddef>
#include <functional>
#include <vector>

// algorithms/ensemblestats —— realization 集合逐像元统计核（纯数值，无 I/O、
// 无 Qt）。统计口径与 catalog/realizationset.h 的 token 一一对应：
//   mean               成员算术均值
//   stddev_population  总体标准差 ÷n——成员等概率即总体（不用样本口径 ÷(n-1)）
//   p10 / p90          升序线性插值分位数（h = q·(n-1)，type-7）
// NaN 语义与 rasteralgebra 同族：逐像元按非 NaN 成员计数 n；
//   n = 0 → 全部输出 NaN（无成员在场不编造）；
//   n = 1 → mean=该值、stddev=0、分位数=该值（单成员口径如实零离散）。
// 惰性纪律：StreamingMoments 逐成员喂入，常驻 O(cells)；quantilesBanded 按
// 行带集齐各成员切片，常驻 = 成员数 × 带行数 × 列数——不全体驻留。
namespace paleo::ensemble
{

struct StatsRequest
{
  bool mean = false;
  bool stddev = false; // 总体口径
  bool p10 = false;
  bool p90 = false;
  bool any() const { return mean || stddev || p10 || p90; }
};

struct StatsResult
{
  // 只填请求的场；未请求的输出为空。长度 = cells。
  std::vector<double> mean;
  std::vector<double> stddev;
  std::vector<double> p10;
  std::vector<double> p90;
  std::vector<int> validCount; // 每像元在场成员数（恒填，供调用方标注覆盖）
};

// 单点分位数：values 可先含 NaN——滤除后升序线性插值（h = q·(n-1)）。
// 有效值空 → NaN。q 裁剪到 [0,1]。
double quantileOf( const double *values, std::size_t n, double q );

// 逐成员流式聚合（均值/总体标准差路径）：调用方一次喂一成员的整幅场。
// 常驻 O(cells)——不驻留成员场。
class StreamingMoments
{
  public:
    explicit StreamingMoments( std::size_t cells = 0 ) { reset( cells ); }
    void reset( std::size_t cells );
    // 长度 == cells；NaN 跳过（不计 n、不进和）。
    void addField( const double *values );
    int fieldsAdded() const { return m_fields; }
    std::vector<double> mean() const;             // n==0 → NaN
    std::vector<double> stddevPopulation() const; // n==0 → NaN；n==1 → 0
    const std::vector<int> &counts() const { return m_n; }
    std::size_t cells() const { return m_cells; }

  private:
    std::size_t m_cells = 0;
    int m_fields = 0;
    std::vector<double> m_sum;
    std::vector<double> m_sumsq;
    std::vector<int> m_n;
};

// 一次性口径（成员场已全部在内存——测试与小场面用；与流式/带式同核）。
StatsResult compute( const std::vector<const double *> &members, std::size_t cells,
                     const StatsRequest &want );

// 带式分位数：每行带把全部成员的切片集齐再逐像元分位，常驻 =
// memberCount × bandRows × cols 个 double。maxBandValues 给常驻上限
//（带行数 = max(1, maxBandValues / (memberCount × cols))）。
// readBand(member, row0, nRows, out) 写 nRows×cols 个 double（nodata 写 NaN）；
// 返回 false 或 cancelled() 为真 → 中止并返回空表。成员数 0 / 网格空 → 空表。
std::vector<std::vector<double>> quantilesBanded(
    std::size_t memberCount, int cols, int rows,
    const std::vector<double> &quantiles, std::size_t maxBandValues,
    const std::function<bool( std::size_t member, int row0, int nRows, double *out )> &readBand,
    const std::function<bool()> &cancelled = {} );

} // namespace paleo::ensemble
