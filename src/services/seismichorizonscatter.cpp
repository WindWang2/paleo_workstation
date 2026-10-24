// 层：数据
#include "seismichorizonscatter.h"
#include "io/segyreader.h"
#include "io/horizonbinner.h"
#include "io/welltable.h"
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

namespace SeismicHorizonDetail {
namespace {
bool readHeader(QFile &input, HorizonHeader *h, SeismicClusterResult *result)
{
  QByteArray header;
  while (!input.atEnd()) {
    const qint64 position = input.pos();
    const auto line = input.readLine();
    if (!line.trimmed().startsWith('#') && !line.trimmed().isEmpty()) {
      input.seek(position); break;
    }
    header += line;
    if (header.size() > 65536) {
      result->error = QStringLiteral("层位文件头过长，无法识别 SMI 格式"); return false;
    }
  }
  if (!parseHorizonHeader(header, h, &result->error)) return false;
  if (h->zUnits.compare(QLatin1String("ms"), Qt::CaseInsensitive) != 0) {
    result->error = QStringLiteral("直接提取需要毫秒（ms）单位的层位时间"); return false;
  }
  const auto hint = paleo::io::wellSourceCrsHint(QString::fromUtf8(header));
  if (!hint.isEmpty()) {
    OGRSpatialReference crs;
    if (crs.SetFromUserInput(hint.toUtf8().constData(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE ||
        !crs.IsProjected() || std::abs(crs.GetLinearUnits() - 1.0) > 1e-9) {
      result->error = QStringLiteral("层位坐标系必须是以米为单位的有效投影坐标系"); return false;
    }
    char *wkt = nullptr;
    if (crs.exportToWkt(&wkt) != OGRERR_NONE) {
      result->error = QStringLiteral("无法读取层位投影坐标系"); return false;
    }
    result->crsWkt = QString::fromUtf8(wkt); CPLFree(wkt);
  }
  return true;
}

// 取窗由原始线道号定位；输出以实际 XY 装箱，供北向上相图及井点投票共同使用。
bool locateCells(Lines *lines, const HorizonHeader &h, SeismicClusterResult *result)
{
  double xmin = std::numeric_limits<double>::infinity(), xmax = -xmin;
  double ymin = xmin, ymax = -xmin;
  for (auto it = lines->cbegin(); it != lines->cend(); ++it)
    for (const auto &s : it.value()) {
      xmin = std::min(xmin, s.x); xmax = std::max(xmax, s.x);
      ymin = std::min(ymin, s.y); ymax = std::max(ymax, s.y);
    }
  const double dx = (xmax - xmin) / (h.gridCols - 1);
  const double dy = (ymax - ymin) / (h.gridRows - 1);
  if (!std::isfinite(dx) || !std::isfinite(dy) || dx <= 0 || dy <= 0) {
    result->error = QStringLiteral("层位与地震缺少有效的二维坐标交集"); return false;
  }
  result->columns = h.gridCols; result->rows = h.gridRows;
  result->extent = QRectF(xmin - dx / 2, ymin - dy / 2, h.gridCols * dx, h.gridRows * dy);
  // 多点落入同一北向上像元时，只取距格心最近的原始样本，不插值或补洞。
  struct Choice { int line, index; double distance; };
  QHash<int, Choice> chosen;
  int collisions = 0;
  for (auto it = lines->begin(); it != lines->end(); ++it)
    for (int i = 0; i < it.value().size(); ++i) {
      auto &s = it.value()[i];
      if (s.timeMs == -9999) { s.cell = -1; continue; }
      const int col = std::clamp(int(std::floor((s.x - result->extent.left()) / dx)), 0, h.gridCols - 1);
      const int row = std::clamp(int(std::floor((result->extent.bottom() - s.y) / dy)), 0, h.gridRows - 1);
      s.cell = row * h.gridCols + col;
      const double distance = std::hypot((s.x - xmin) / dx - col, (ymax - s.y) / dy - row);
      auto old = chosen.find(s.cell);
      if (old != chosen.end()) {
        ++collisions;
        if (distance >= old->distance) { s.cell = -1; continue; }
        (*lines)[old->line][old->index].cell = -1;
      }
      chosen.insert(s.cell, {it.key(), i, distance});
    }
  for (auto &samples : *lines)
    samples.erase(std::remove_if(samples.begin(), samples.end(), [](const Sample &s) { return s.cell < 0; }), samples.end());
  if (collisions)
    result->warnings << QStringLiteral("有 %1 个层位点落入同一输出像元，保留距格心最近的原始点").arg(collisions);
  return true;
}
}

bool readScatter(const QString &path, const SegyReader &reader, Lines *lines,
                 SeismicClusterResult *result, const std::function<void(int)> &progress,
                 const std::function<bool()> &cancel)
{
  QFile input(path);
  if (!input.open(QIODevice::ReadOnly)) {
    result->error = QStringLiteral("无法读取层位文件：%1").arg(input.errorString()); return false;
  }
  HorizonHeader h;
  if (!readHeader(input, &h, result)) return false;
  const auto g = reader.geometry();
  // 已投影数据可能经历整数道头坐标的重投影/再次取整。按测网最小间距的
  // 10% 容许这类亚道间距误差；局部测网仍保留 1m 口径，不放行整道错位。
  const double columnStep = std::min(std::hypot(g.cornerX[1] - g.cornerX[0], g.cornerY[1] - g.cornerY[0]),
      std::hypot(g.cornerX[2] - g.cornerX[3], g.cornerY[2] - g.cornerY[3])) / (double(g.xlineMax) - g.xlineMin);
  const double rowStep = std::min(std::hypot(g.cornerX[3] - g.cornerX[0], g.cornerY[3] - g.cornerY[0]),
      std::hypot(g.cornerX[2] - g.cornerX[1], g.cornerY[2] - g.cornerY[1])) / (double(g.inlineMax) - g.inlineMin);
  const double baseTolerance = result->crsWkt.isEmpty() ? 1.0 : std::max(1.0, 0.1 * std::min(columnStep, rowStep));
  QSet<quint64> occupied;
  const QRegularExpression spaces(QStringLiteral("\\s+"));
  int readRows = 0;
  while (!input.atEnd()) {
    if ((++readRows % 1024) == 0) {
      if (cancel && cancel()) { result->error = QStringLiteral("已取消"); return false; }
      if (progress) progress(20 + int(5.0 * input.pos() / std::max<qint64>(1, input.size())));
    }
    const auto text = QString::fromUtf8(input.readLine()).trimmed();
    if (text.isEmpty() || text.startsWith('#')) continue;
    const auto fields = text.split(spaces, Qt::SkipEmptyParts);
    if (fields.size() < 5) continue;
    bool xOk, yOk, timeOk, ilOk, xlOk;
    const double x = fields[0].toDouble(&xOk), y = fields[1].toDouble(&yOk), time = fields[2].toDouble(&timeOk);
    const int il = fields[3].toInt(&ilOk), xl = fields[4].toInt(&xlOk);
    if (!xOk || !yOk || !timeOk || !ilOk || !xlOk || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(time) || il < h.p1Inline || il > h.p3Inline ||
        xl < h.p1Xline || xl > h.p2Xline || il < g.inlineMin || il > g.inlineMax ||
        xl < g.xlineMin || xl > g.xlineMax) continue;
    const quint64 key = (quint64(quint32(il)) << 32) | quint32(xl);
    if (occupied.contains(key)) {
      result->error = QStringLiteral("层位文件有重复的 Inline/Crossline 位置，请先处理重复点"); return false;
    }
    occupied.insert(key);
    (*lines)[il].append({0, xl, time, x, y});
  }
  if (input.error() != QFileDevice::NoError) {
    result->error = QStringLiteral("读取层位文件失败：%1").arg(input.errorString()); return false;
  }
  SegyOptions options; options.cancel = cancel;
  int lineCount = 0;
  double maxDistance = 0;
  for (auto it = lines->begin(); it != lines->end(); ++it) {
    QHash<qint32, SegyReader::TraceCoordinate> coordinates;
    if (!reader.readInlineCoordinates(it.key(), &coordinates, &result->error, &options)) return false;
    auto &samples = it.value();
    for (auto &s : samples) {
      const auto trace = coordinates.constFind(s.xline);
      if (trace == coordinates.cend()) { s.cell = -1; continue; }
      const double distance = std::hypot(s.x - trace->x, s.y - trace->y);
      maxDistance = std::max(maxDistance, distance);
      const double tolerance = std::max(baseTolerance, trace->resolution / std::sqrt(2.0) + 0.001);
      if (distance > tolerance) {
        result->error = QStringLiteral("层位坐标与所选地震道不一致（inline %1，crossline %2，偏差 %3 米，容差 %4 米），请核对测区及坐标系")
            .arg(it.key()).arg(s.xline).arg(distance, 0, 'f', 3).arg(tolerance, 0, 'f', 3);
        return false;
      }
    }
    samples.erase(std::remove_if(samples.begin(), samples.end(), [](const Sample &s) { return s.cell < 0; }), samples.end());
    if (progress) progress(25 + 5 * ++lineCount / std::max(1, int(lines->size())));
  }
  if (maxDistance > 1.0)
    result->warnings << QStringLiteral("层位与实际地震道坐标最大偏差 %1 米，已按测网间距及道头量化精度核验")
        .arg(maxDistance, 0, 'f', 3);
  return locateCells(lines, h, result);
}
}
