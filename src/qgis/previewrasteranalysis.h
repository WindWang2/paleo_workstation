// 层：QGIS 封装
#pragma once
#include <functional>
#include <vector>
class PaleoTaskService;
class QObject;

#include <QColor>
#include <QPair>
#include <QString>
#include <QVector>

#include <qgspointxy.h>
#include <qgsrectangle.h>

class QgsRasterLayer;

// qgis/previewrasteranalysis — 预览栅格的读侧分析件（P2 D2.2/D2.3/D5.5–5.8/
// D2.11）。纯计算无 UI：统计/直方图/拉伸界/剖面采样/局部极值/金字塔探测，
// 以及带「空 itemList 必须先 classify」地雷防护的伪彩渲染出口。
namespace PreviewRasterAnalysis
{

// ---- D5.5 波段统计 ----
struct BandSummary
{
  bool valid = false;
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  double stdDev = 0.0;
  double variance() const { return stdDev * stdDev; }
  qint64 validPixels = 0;
  qint64 totalPixels = 0;
  double validRatio() const { return totalPixels > 0 ? double( validPixels ) / totalPixels : 0.0; }
};
BandSummary summarize( QgsRasterLayer *layer, int band = 1 );
void requestSummary(const QString &uri, PaleoTaskService *tasks,
                    QObject *receiver,
                    std::function<void(const BandSummary &)> ready);

// ---- D2.3/D5.8 直方图 ----
struct Histogram
{
  bool valid = false;
  int bins = 0;
  double lo = 0.0;
  double hi = 0.0;
  QVector<qint64> counts;
  qint64 totalCount() const;
  double maximumCount() const;
};
Histogram histogram( QgsRasterLayer *layer, int bins = 64, int band = 1 );
void requestHistogram(const QString &uri, int bins, PaleoTaskService *tasks,
                      QObject *receiver,
                      std::function<void(const Histogram &)> ready);

// ---- D2.2 拉伸 ----
enum class Stretch
{
  MinMax,     // 全值域
  Percent2To98, // 2%–98% 分位截断
  HistEq,     // 直方图均衡（分位数断点分类）
  Manual      // 调用方给值（本函数不处理，透传）
};
// 由直方图求连续渲染的上下界（Manual 透传 caller 值；无效直方图回退 0..1）。
QPair<double, double> stretchBounds( const Histogram &h, Stretch mode,
                                     double manualLo = 0.0, double manualHi = 1.0 );

// ---- D2.2 色带预设（≥4 档；bipolar 指双极色带用于含负值数据）----
struct RampPreset
{
  QString id;
  QString name;
  QColor c1;       // 低值端
  QColor c2;       // 高值端
  QColor mid;      // 双极的零值色（单极无效）
  bool bipolar = false;
};
const QVector<RampPreset> &rampPresets();
const RampPreset *rampPreset( const QString &id );

// ---- D2.1/D2.2 安全伪彩渲染出口 ----
// Continuous：QgsColorRampShader 先 classifyColorRamp（空 itemList 地雷防护）。
// Quantile：按直方图分位数放 N 个离散断点（HistEq 语义的渲染落地）。
enum class Classification { Continuous, Quantile };
bool applyPseudoColorRenderer( QgsRasterLayer *layer, int band, double lo, double hi,
                               const RampPreset &preset, bool invert = false,
                               Classification cls = Classification::Continuous,
                               int quantileClasses = 16 );

// ---- D5.1 剖面采样 ----
struct ProfileSample
{
  double distance = 0.0;  // 距 p1 的平面距离（米）
  double value = 0.0;
  bool valid = false;     // 落在栅格外/NODATA → false，不造假值
};
QVector<ProfileSample> sampleProfile( QgsRasterLayer *layer, const QgsPointXY &p1,
                                      const QgsPointXY &p2, int count = 200 );

// ---- D5.6 局部极值 ----
struct Extremum
{
  QgsPointXY pos;
  double value = 0.0;
  bool peak = false; // true=峰，false=洼地
};
// window×window 邻域极值 + prominence（相对邻域均值的显著性，单位=值）。
QVector<Extremum> localExtrema(QgsRasterLayer *layer, int band = 1,
                               int window = 5, double prominence = 0.0,
                               int maxCount = 200,
                               const std::function<bool()> &cancelled = {});

// ---- 稳健显著性尺度（纯计算，无 QGIS 依赖，便于合成栅格单测）----
// 极值的「显著性」= |v − 邻域有效像元均值|（即局部残差）。阈值不能用全局
// σ：全局 σ 量的是整面的区域起伏（构造趋势），而 window 内残差量的是局部
// 粗糙度，二者量纲相同但尺度可差一到两个数量级——平滑层位面上全局 σ×0.5
// 会滤光所有真实极值。改用「全部有效像元局部残差」的稳健尺度：
//   scale = 1.4826·MAD(r)            （正态下与 σ 一致，抗极值/尖峰污染）
//   MAD=0（多数像元残差相同，如平面上少量尖峰）→ 1.2533·mean|r − median(r)|
//   仍为 0（完全平坦/无残差）→ 0（此时也不存在严格极值）
// 阈值 = k·scale，默认 k=3（残差分布的 3σ 离群口径）。全程确定性：
// 固定扫描序 + nth_element 求中位数 + 稳定排序（显著性降序，再按行列）。
struct ExtremaGrid
{
  int width = 0;
  int height = 0;
  std::vector<double> values;       // 行主序，width*height
  std::vector<unsigned char> valid; // 0 = nodata / 非有限
  bool ok(int x, int y) const { return valid[std::size_t(y) * width + x] != 0; }
  double at(int x, int y) const { return values[std::size_t(y) * width + x]; }
};
struct GridExtremum
{
  int x = 0;
  int y = 0;
  double value = 0.0;
  double prominence = 0.0;
  bool peak = false;
};
constexpr double kRobustProminenceK = 3.0;
double robustResidualScale(const ExtremaGrid &grid, int window = 5,
                           const std::function<bool()> &cancelled = {});
std::vector<GridExtremum> gridExtrema(const ExtremaGrid &grid, int window,
                                      double minProminence, int maxCount,
                                      const std::function<bool()> &cancelled = {});
// 稳健阈值版：minProminence = k·robustResidualScale。
std::vector<GridExtremum> robustGridExtrema(const ExtremaGrid &grid, int window = 5,
                                            double k = kRobustProminenceK,
                                            int maxCount = 100,
                                            const std::function<bool()> &cancelled = {});
QVector<Extremum> robustLocalExtrema(QgsRasterLayer *layer, int band = 1,
                                     int window = 5, double k = kRobustProminenceK,
                                     int maxCount = 100,
                                     const std::function<bool()> &cancelled = {});

void requestExtrema(const QString &uri, PaleoTaskService *tasks,
                    QObject *receiver,
                    std::function<void(const QVector<Extremum> &)> ready);
void requestProfile(const QString &uri, const QgsPointXY &p1,
                    const QgsPointXY &p2, PaleoTaskService *tasks,
                    QObject *receiver,
                    std::function<void(const QVector<ProfileSample> &)> ready);
// ---- D2.11 大图降级情报 ----
bool hasPyramids( QgsRasterLayer *layer );
qint64 fileSizeBytes( const QString &path );
// 大图提示文案（中文，视图直接展示）：>50MB 且无金字塔时非空。
QString bigRasterHint( QgsRasterLayer *layer );

} // namespace PreviewRasterAnalysis
