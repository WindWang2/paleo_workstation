// 层：数据
#pragma once

#include "stratgrid.h"
#include "upscale.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <functional>
#include <vector>

// propfill — 地层坐标 IDW 充填 + 断层阻断（纯数值）。
//
// 距离在 IJK 指标空间：d² = Δi² + Δj² + Δk²，权重 1/d^power（power 缺省 2）。
// 命中种子所在单元取该种子（多个精确命中取平均），不再与远处种子混合。
//
// 断层是地图 XY 上的竖直面帘（本轮不做断块错位）。柱心连线与任一段相交
// 则两柱不连通；块内才互相插值。没有种子的块保持 NaN，不跨断层补值。
// 进度回调返回 false 表示取消：失败且 *out 不写入半成品。

namespace paleo::stratgrid
{

struct Seed
{
  int i = 0;
  int j = 0;
  int k = 0;
  double value = 0;
};

struct FaultSegment
{
  double x0 = 0;
  double y0 = 0;
  double x1 = 0;
  double y1 = 0;
};

struct PropertyVolume
{
  ZoneGrid grid;
  std::vector<float> values;     // cellIndex，NaN = 死单元或未充填
  std::vector<int> columnBlock;  // ni*nj，死柱 = -1
  int blockCount = 0;
  int filledCells = 0;
  int unfilledLiveCells = 0;
};

// fraction ∈ [0,1]。返回 false 取消。
using FillProgress = std::function<bool(double fraction)>;

bool fillIdw(const ZoneGrid &grid, const std::vector<Seed> &seeds,
             const std::vector<FaultSegment> &faults, double power, PropertyVolume *out,
             const FillProgress &progress = {}, QString *error = nullptr);

// 粗化表 → 种子（无值或没有代表柱的层跳过）。
std::vector<Seed> seedsFromUpscale(const UpscaleTable &table);

// 剖面投影：每个采样点 (trace, sample) 用该道的地图 (x,y) 与 z=z0+sample*dz
// 反查 IJK，取出属性。落在死柱 / 层段外 → NaN。
// out 布局与剖面画布一致：行 = 采样（y），列 = 道（x），下标 sample*nTraces+trace。
struct SectionGeometry
{
  int nTraces = 0;
  int nSamples = 0;
  std::vector<double> traceX;
  std::vector<double> traceY;
  double z0 = 0;
  double dz = 1;
};

bool projectToSection(const PropertyVolume &volume, const SectionGeometry &section,
                      std::vector<float> *out, float *valueMin, float *valueMax,
                      QString *error = nullptr);

// axis 0 = 固定 i（宽 nj、高 nk）；1 = 固定 j（宽 ni、高 nk）；2 = 固定 k（宽 ni、高 nj）。
// 图像行优先，行 = 第二轴从 0 增长（k 或 j）。
bool extractSlice(const PropertyVolume &volume, int axis, int index, std::vector<float> *out,
                  int *width, int *height, float *valueMin, float *valueMax, QString *error = nullptr);

// 自描述小端体：magic "PPROP1\\n" + u32 json 长度 + json + top/bot/values float32。
// 无时间戳，同输入字节级稳定。
QByteArray writePropertyBlob(const PropertyVolume &volume, const QJsonObject &provenance);
bool readPropertyBlob(const QByteArray &blob, PropertyVolume *volume, QJsonObject *provenance,
                      QString *error = nullptr);

} // namespace paleo::stratgrid
