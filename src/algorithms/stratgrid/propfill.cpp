// 层：数据
#include "propfill.h"

#include <QJsonDocument>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <queue>

namespace paleo::stratgrid
{
namespace
{

void setError(QString *error, const QString &text)
{
  if (error)
    *error = text;
}

int orient(double ax, double ay, double bx, double by, double cx, double cy)
{
  const double v = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
  const double scale = 1.0 + std::fabs(ax) + std::fabs(ay) + std::fabs(bx) + std::fabs(by) +
                       std::fabs(cx) + std::fabs(cy);
  const double eps = 1e-9 * scale;
  if (v > eps)
    return 1;
  if (v < -eps)
    return -1;
  return 0;
}

bool onSegment(double ax, double ay, double bx, double by, double px, double py)
{
  const double eps = 1e-9 * (1.0 + std::fabs(ax) + std::fabs(bx) + std::fabs(px));
  return px >= std::min(ax, bx) - eps && px <= std::max(ax, bx) + eps &&
         py >= std::min(ay, by) - eps && py <= std::max(ay, by) + eps;
}

bool segmentsCross(double ax, double ay, double bx, double by, double cx, double cy, double dx,
                   double dy)
{
  const int o1 = orient(ax, ay, bx, by, cx, cy);
  const int o2 = orient(ax, ay, bx, by, dx, dy);
  const int o3 = orient(cx, cy, dx, dy, ax, ay);
  const int o4 = orient(cx, cy, dx, dy, bx, by);
  if (o1 == 0 && o2 == 0 && o3 == 0 && o4 == 0)
  {
    return (onSegment(ax, ay, bx, by, cx, cy) && onSegment(ax, ay, bx, by, dx, dy)) ||
           (onSegment(cx, cy, dx, dy, ax, ay) && onSegment(cx, cy, dx, dy, bx, by)) ||
           (onSegment(ax, ay, bx, by, cx, cy) && onSegment(cx, cy, dx, dy, ax, ay)) ||
           (onSegment(ax, ay, bx, by, dx, dy) && onSegment(cx, cy, dx, dy, bx, by));
  }
  return o1 * o2 < 0 && o3 * o4 < 0;
}

void columnCenter(const ZoneGrid &grid, int i, int j, double *x, double *y)
{
  *x = grid.originX + (static_cast<double>(i) + 0.5) * grid.dx;
  *y = grid.originY + (static_cast<double>(j) + 0.5) * grid.dy;
}

bool edgeBlocked(const ZoneGrid &grid, int i0, int j0, int i1, int j1,
                 const std::vector<FaultSegment> &faults)
{
  double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  columnCenter(grid, i0, j0, &x0, &y0);
  columnCenter(grid, i1, j1, &x1, &y1);
  for (const FaultSegment &f : faults)
  {
    if (segmentsCross(x0, y0, x1, y1, f.x0, f.y0, f.x1, f.y1))
      return true;
  }
  return false;
}

void assignBlocks(const ZoneGrid &grid, const std::vector<FaultSegment> &faults,
                  std::vector<int> *columnBlock, int *blockCount)
{
  const int n = grid.ni * grid.nj;
  columnBlock->assign(static_cast<std::size_t>(n), -1);
  int next = 0;
  const int di[4] = {-1, 1, 0, 0};
  const int dj[4] = {0, 0, -1, 1};
  for (int j = 0; j < grid.nj; ++j)
  {
    for (int i = 0; i < grid.ni; ++i)
    {
      const int start = grid.columnIndex(i, j);
      if (!grid.columnLive(i, j) || (*columnBlock)[static_cast<std::size_t>(start)] >= 0)
        continue;
      std::queue<std::pair<int, int>> q;
      q.emplace(i, j);
      (*columnBlock)[static_cast<std::size_t>(start)] = next;
      while (!q.empty())
      {
        const auto [ci, cj] = q.front();
        q.pop();
        for (int nEdge = 0; nEdge < 4; ++nEdge)
        {
          const int ni = ci + di[nEdge];
          const int nj = cj + dj[nEdge];
          if (ni < 0 || nj < 0 || ni >= grid.ni || nj >= grid.nj || !grid.columnLive(ni, nj))
            continue;
          const int idx = grid.columnIndex(ni, nj);
          if ((*columnBlock)[static_cast<std::size_t>(idx)] >= 0)
            continue;
          if (edgeBlocked(grid, ci, cj, ni, nj, faults))
            continue;
          (*columnBlock)[static_cast<std::size_t>(idx)] = next;
          q.emplace(ni, nj);
        }
      }
      ++next;
    }
  }
  *blockCount = next;
}

void trackRange(float v, bool *any, float *vmin, float *vmax)
{
  if (!std::isfinite(v))
    return;
  if (!*any)
  {
    *vmin = v;
    *vmax = v;
    *any = true;
  }
  else
  {
    *vmin = std::min(*vmin, v);
    *vmax = std::max(*vmax, v);
  }
}

} // namespace

std::vector<Seed> seedsFromUpscale(const UpscaleTable &table)
{
  std::vector<Seed> seeds;
  for (int w = 0; w < table.nWells; ++w)
  {
    for (int k = 0; k < table.nLayers; ++k)
    {
      const LayerValue &cell = table.at(w, k);
      if (!cell.hasValue || cell.columnI < 0 || cell.columnJ < 0)
        continue;
      seeds.push_back(Seed{cell.columnI, cell.columnJ, k, cell.value});
    }
  }
  return seeds;
}

bool fillIdw(const ZoneGrid &grid, const std::vector<Seed> &seeds,
             const std::vector<FaultSegment> &faults, double power, PropertyVolume *out,
             const FillProgress &progress, QString *error)
{
  if (!out)
  {
    setError(error, QStringLiteral("充填输出为空"));
    return false;
  }
  if (!(power > 0.0) || !std::isfinite(power))
  {
    setError(error, QStringLiteral("IDW 幂次须为正有限值"));
    return false;
  }
  if (grid.nk < 1 || grid.ni < 1 || grid.nj < 1)
  {
    setError(error, QStringLiteral("格架为空"));
    return false;
  }

  PropertyVolume local;
  local.grid = grid;
  assignBlocks(grid, faults, &local.columnBlock, &local.blockCount);
  const float kNan = std::numeric_limits<float>::quiet_NaN();
  local.values.assign(static_cast<std::size_t>(grid.cellCount()), kNan);

  std::vector<std::vector<Seed>> byBlock(static_cast<std::size_t>(std::max(local.blockCount, 0)));
  for (const Seed &seed : seeds)
  {
    if (!grid.columnLive(seed.i, seed.j) || seed.k < 0 || seed.k >= grid.nk ||
        !std::isfinite(seed.value))
      continue;
    const int block = local.columnBlock[static_cast<std::size_t>(grid.columnIndex(seed.i, seed.j))];
    if (block < 0)
      continue;
    byBlock[static_cast<std::size_t>(block)].push_back(seed);
  }

  if (progress && !progress(0.0))
  {
    setError(error, QStringLiteral("已取消"));
    return false;
  }

  const int rows = grid.nj;
  int filled = 0;
  int unfilled = 0;
  for (int j = 0; j < grid.nj; ++j)
  {
    for (int i = 0; i < grid.ni; ++i)
    {
      if (!grid.columnLive(i, j))
        continue;
      const int block = local.columnBlock[static_cast<std::size_t>(grid.columnIndex(i, j))];
      if (block < 0 || byBlock[static_cast<std::size_t>(block)].empty())
      {
        unfilled += grid.nk;
        continue;
      }
      const std::vector<Seed> &bucket = byBlock[static_cast<std::size_t>(block)];
      for (int k = 0; k < grid.nk; ++k)
      {
        const int idx = grid.cellIndex(i, j, k);
        double exactSum = 0;
        int exactN = 0;
        double wsum = 0;
        double vsum = 0;
        for (const Seed &seed : bucket)
        {
          const double di = static_cast<double>(i - seed.i);
          const double dj = static_cast<double>(j - seed.j);
          const double dk = static_cast<double>(k - seed.k);
          const double d2 = di * di + dj * dj + dk * dk;
          if (d2 == 0.0)
          {
            exactSum += seed.value;
            ++exactN;
            continue;
          }
          const double w = 1.0 / std::pow(d2, power * 0.5);
          wsum += w;
          vsum += w * seed.value;
        }
        float value = kNan;
        if (exactN > 0)
          value = static_cast<float>(exactSum / static_cast<double>(exactN));
        else if (wsum > 0.0)
          value = static_cast<float>(vsum / wsum);
        local.values[static_cast<std::size_t>(idx)] = value;
        if (std::isfinite(value))
          ++filled;
        else
          ++unfilled;
      }
    }
    if (progress)
    {
      const double fraction = static_cast<double>(j + 1) / static_cast<double>(rows);
      if (!progress(fraction))
      {
        setError(error, QStringLiteral("已取消"));
        return false;
      }
    }
  }
  local.filledCells = filled;
  local.unfilledLiveCells = unfilled;
  *out = std::move(local);
  return true;
}

bool projectToSection(const PropertyVolume &volume, const SectionGeometry &section,
                      std::vector<float> *out, float *valueMin, float *valueMax, QString *error)
{
  if (!out)
  {
    setError(error, QStringLiteral("剖面投影输出为空"));
    return false;
  }
  if (section.nTraces < 1 || section.nSamples < 1 ||
      static_cast<int>(section.traceX.size()) != section.nTraces ||
      static_cast<int>(section.traceY.size()) != section.nTraces || !(section.dz != 0.0))
  {
    setError(error, QStringLiteral("剖面几何不完整"));
    return false;
  }
  const float kNan = std::numeric_limits<float>::quiet_NaN();
  out->assign(static_cast<std::size_t>(section.nTraces * section.nSamples), kNan);
  bool any = false;
  float vmin = 0, vmax = 0;
  const ZoneGrid &grid = volume.grid;
  for (int s = 0; s < section.nSamples; ++s)
  {
    const double z = section.z0 + static_cast<double>(s) * section.dz;
    for (int t = 0; t < section.nTraces; ++t)
    {
      Index3 ijk;
      if (!ijkAt(grid, section.traceX[static_cast<std::size_t>(t)],
                 section.traceY[static_cast<std::size_t>(t)], z, &ijk))
        continue;
      if (volume.values.size() != static_cast<std::size_t>(grid.cellCount()))
        continue;
      const float v = volume.values[static_cast<std::size_t>(grid.cellIndex(ijk.i, ijk.j, ijk.k))];
      (*out)[static_cast<std::size_t>(s * section.nTraces + t)] = v;
      trackRange(v, &any, &vmin, &vmax);
    }
  }
  if (valueMin)
    *valueMin = any ? vmin : 0;
  if (valueMax)
    *valueMax = any ? vmax : 0;
  return true;
}

bool extractSlice(const PropertyVolume &volume, int axis, int index, std::vector<float> *out,
                  int *width, int *height, float *valueMin, float *valueMax, QString *error)
{
  if (!out || !width || !height)
  {
    setError(error, QStringLiteral("切片输出为空"));
    return false;
  }
  const ZoneGrid &g = volume.grid;
  if (volume.values.size() != static_cast<std::size_t>(g.cellCount()))
  {
    setError(error, QStringLiteral("属性体与格架尺寸不符"));
    return false;
  }
  int w = 0, h = 0;
  if (axis == 0)
  {
    if (index < 0 || index >= g.ni)
    {
      setError(error, QStringLiteral("I 切片越界"));
      return false;
    }
    w = g.nj;
    h = g.nk;
  }
  else if (axis == 1)
  {
    if (index < 0 || index >= g.nj)
    {
      setError(error, QStringLiteral("J 切片越界"));
      return false;
    }
    w = g.ni;
    h = g.nk;
  }
  else if (axis == 2)
  {
    if (index < 0 || index >= g.nk)
    {
      setError(error, QStringLiteral("K 切片越界"));
      return false;
    }
    w = g.ni;
    h = g.nj;
  }
  else
  {
    setError(error, QStringLiteral("切片轴须为 0/1/2"));
    return false;
  }
  const float kNan = std::numeric_limits<float>::quiet_NaN();
  out->assign(static_cast<std::size_t>(w * h), kNan);
  bool any = false;
  float vmin = 0, vmax = 0;
  for (int row = 0; row < h; ++row)
  {
    for (int col = 0; col < w; ++col)
    {
      int i = 0, j = 0, k = 0;
      if (axis == 0)
      {
        i = index;
        j = col;
        k = row;
      }
      else if (axis == 1)
      {
        i = col;
        j = index;
        k = row;
      }
      else
      {
        i = col;
        j = row;
        k = index;
      }
      float v = kNan;
      if (g.columnLive(i, j))
        v = volume.values[static_cast<std::size_t>(g.cellIndex(i, j, k))];
      (*out)[static_cast<std::size_t>(row * w + col)] = v;
      trackRange(v, &any, &vmin, &vmax);
    }
  }
  *width = w;
  *height = h;
  if (valueMin)
    *valueMin = any ? vmin : 0;
  if (valueMax)
    *valueMax = any ? vmax : 0;
  return true;
}

QByteArray writePropertyBlob(const PropertyVolume &volume, const QJsonObject &provenance)
{
  static_assert(std::endian::native == std::endian::little, "PPROP1 stores little-endian floats");
  const ZoneGrid &g = volume.grid;
  const int n = g.ni * g.nj;
  if (n < 1 || g.nk < 1 || static_cast<int>(g.topZ.size()) != n ||
      static_cast<int>(g.botZ.size()) != n ||
      static_cast<int>(volume.values.size()) != g.cellCount())
    return {};
  QJsonObject obj;
  obj.insert(QStringLiteral("format"), QStringLiteral("paleo-property-volume"));
  obj.insert(QStringLiteral("version"), 1);
  obj.insert(QStringLiteral("ni"), g.ni);
  obj.insert(QStringLiteral("nj"), g.nj);
  obj.insert(QStringLiteral("nk"), g.nk);
  obj.insert(QStringLiteral("originX"), g.originX);
  obj.insert(QStringLiteral("originY"), g.originY);
  obj.insert(QStringLiteral("dx"), g.dx);
  obj.insert(QStringLiteral("dy"), g.dy);
  obj.insert(QStringLiteral("liveColumns"), g.liveColumns);
  obj.insert(QStringLiteral("filledCells"), volume.filledCells);
  obj.insert(QStringLiteral("unfilledLiveCells"), volume.unfilledLiveCells);
  obj.insert(QStringLiteral("provenance"), provenance);
  const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
  QByteArray blob;
  blob.reserve(static_cast<int>(json.size() + 16 + (n * 2 + g.cellCount()) * 4));
  blob.append("PPROP1\n", 7);
  char len[4];
  qToLittleEndian(static_cast<quint32>(json.size()), len);
  blob.append(len, 4);
  blob.append(json);
  const auto append = [&](const std::vector<float> &values) {
    blob.append(reinterpret_cast<const char *>(values.data()),
                static_cast<int>(values.size() * sizeof(float)));
  };
  append(g.topZ);
  append(g.botZ);
  append(volume.values);
  return blob;
}

bool readPropertyBlob(const QByteArray &blob, PropertyVolume *volume, QJsonObject *provenance,
                      QString *error)
{
  static_assert(std::endian::native == std::endian::little, "PPROP1 stores little-endian floats");
  if (!volume)
  {
    setError(error, QStringLiteral("读回输出为空"));
    return false;
  }
  *volume = PropertyVolume{};
  if (blob.size() < 11 || std::memcmp(blob.constData(), "PPROP1\n", 7) != 0)
  {
    setError(error, QStringLiteral("不是属性体文件（缺少 PPROP1 头）"));
    return false;
  }
  const quint32 jsonLen = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(blob.constData() + 7));
  if (jsonLen == 0 || 11 + static_cast<int>(jsonLen) > blob.size())
  {
    setError(error, QStringLiteral("属性体头长度损坏"));
    return false;
  }
  const QByteArray json = blob.mid(11, static_cast<int>(jsonLen));
  QJsonParseError parseErr;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &parseErr);
  if (parseErr.error != QJsonParseError::NoError || !doc.isObject())
  {
    setError(error, QStringLiteral("属性体头不是 JSON 对象"));
    return false;
  }
  const QJsonObject obj = doc.object();
  if (obj.value(QStringLiteral("format")).toString() != QLatin1String("paleo-property-volume") ||
      obj.value(QStringLiteral("version")).toInt() != 1)
  {
    setError(error, QStringLiteral("属性体格式版本不认识"));
    return false;
  }
  ZoneGrid grid;
  grid.ni = obj.value(QStringLiteral("ni")).toInt();
  grid.nj = obj.value(QStringLiteral("nj")).toInt();
  grid.nk = obj.value(QStringLiteral("nk")).toInt();
  grid.originX = obj.value(QStringLiteral("originX")).toDouble();
  grid.originY = obj.value(QStringLiteral("originY")).toDouble();
  grid.dx = obj.value(QStringLiteral("dx")).toDouble();
  grid.dy = obj.value(QStringLiteral("dy")).toDouble();
  const int n = grid.ni * grid.nj;
  const int cells = n * grid.nk;
  const int need = 11 + static_cast<int>(jsonLen) + (n * 2 + cells) * static_cast<int>(sizeof(float));
  if (grid.ni < 1 || grid.nj < 1 || grid.nk < 1 || blob.size() < need)
  {
    setError(error, QStringLiteral("属性体几何或体数据长度不符"));
    return false;
  }
  const char *cursor = blob.constData() + 11 + static_cast<int>(jsonLen);
  const auto readFloats = [&](int count, std::vector<float> *dst) {
    dst->resize(static_cast<std::size_t>(count));
    std::memcpy(dst->data(), cursor, static_cast<std::size_t>(count) * sizeof(float));
    cursor += static_cast<std::size_t>(count) * sizeof(float);
  };
  readFloats(n, &grid.topZ);
  readFloats(n, &grid.botZ);
  grid.live.assign(static_cast<std::size_t>(n), 0);
  int live = 0;
  for (int i = 0; i < n; ++i)
  {
    const float tz = grid.topZ[static_cast<std::size_t>(i)];
    const float bz = grid.botZ[static_cast<std::size_t>(i)];
    if (std::isfinite(tz) && std::isfinite(bz) &&
        static_cast<double>(bz) - static_cast<double>(tz) > kThicknessEps)
    {
      grid.live[static_cast<std::size_t>(i)] = 1;
      ++live;
    }
  }
  grid.liveColumns = live;
  grid.deadColumns = n - live;
  volume->grid = std::move(grid);
  readFloats(cells, &volume->values);
  volume->filledCells = obj.value(QStringLiteral("filledCells")).toInt();
  volume->unfilledLiveCells = obj.value(QStringLiteral("unfilledLiveCells")).toInt();
  if (provenance)
  {
    const QJsonValue prov = obj.value(QStringLiteral("provenance"));
    *provenance = prov.isObject() ? prov.toObject() : QJsonObject();
  }
  return true;
}

} // namespace paleo::stratgrid
