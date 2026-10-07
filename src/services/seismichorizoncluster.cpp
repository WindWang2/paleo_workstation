// 层：数据
#include "seismichorizoncluster.h"
#include "io/segyreader.h"
#include "services/seismicmapping.h"
#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>
#include <gdal.h>
#include <limits>
#include <memory>
#include <numeric>

SeismicClusterResult clusterSeismicHorizon(
    const QString &seismicPath, const QString &horizonPath,
    const QString &cacheDir, const QVector<int> &codes,
    const std::function<void(int)> &progress,
    const std::function<bool()> &cancel, SeismicHorizonSource source) {
  SeismicClusterResult result;
  const auto stopped = [&] { return cancel && cancel(); };
  const auto report = [&](int value) { if (progress) progress(value); };
  if (stopped()) {
    result.error = QStringLiteral("已取消");
    return result;
  }
  if (codes.isEmpty() || codes.size() > 64) {
    result.error = QStringLiteral("请配置 1–64 个相类别");
    return result;
  }
  struct Sample { int cell, xline; double timeMs; };
  QMap<int, QVector<Sample>> lines;
  if (source == SeismicHorizonSource::TimeRaster) {
    GDALAllRegister();
    using Dataset = std::unique_ptr<void, decltype(&GDALClose)>;
    Dataset ds(GDALOpen(horizonPath.toUtf8().constData(), GA_ReadOnly), GDALClose);
    double gt[6];
    if (!ds || GDALGetGeoTransform(ds.get(), gt) != CE_None ||
        gt[1] <= 0 || gt[5] >= 0 || gt[2] != 0 || gt[4] != 0) {
      result.error = QStringLiteral("当前层位缺少可用的北向上 TWT 栅格");
      return result;
    }
    const int width = GDALGetRasterXSize(ds.get()), height = GDALGetRasterYSize(ds.get());
    result.extent = QRectF(gt[0], gt[3] + height * gt[5], width * gt[1], -height * gt[5]);
    if (width <= 0 || height <= 0 || result.extent.isEmpty()) {
      result.error = QStringLiteral("层位网格范围为空");
      return result;
    }
    result.columns = std::min(64, width);
    result.rows = std::min(64, height);
    const int count = result.columns * result.rows;
    QVector<float> times(count);
    auto band = GDALGetRasterBand(ds.get(), 1);
    int hasNoData = 0;
    const double noData = GDALGetRasterNoDataValue(band, &hasNoData);
    if (GDALRasterIO(band, GF_Read, 0, 0, width, height, times.data(),
                     result.columns, result.rows, GDT_Float32, 0, 0) != CE_None) {
      result.error = QStringLiteral("读取层位时间网格失败");
      return result;
    }
    // Raster 路径的坐标定位在地震测网载入后进行。
    // 用临时样点保存像元时间，不把其它栅格解释为层位。
    for (int row = 0; row < result.rows; ++row)
      for (int col = 0; col < result.columns; ++col) {
        const int cell = row * result.columns + col;
        if (std::isfinite(times[cell]) && !(hasNoData && times[cell] == noData))
          lines[0].append({cell, 0, times[cell]});
      }
  }
  SegyReader reader;
  SegyOptions options;
  options.cancel = cancel;
  options.progress = [&](qint64 done, qint64 total) {
    report(total > 0 ? int(20.0 * done / total) : 0);
  };
  if (!reader.openCached(seismicPath, cacheDir, &result.error, &options))
    return result;
  options.progress = {}; // 解码进度由本任务按测线汇总，避免每条线回退至 0%。
  const auto g = reader.geometry();
  const double di = double(g.inlineMax) - g.inlineMin, dx = double(g.xlineMax) - g.xlineMin;
  if (di <= 0 || dx <= 0) {
    result.error = QStringLiteral("地震体号域不构成二维测网");
    return result;
  }
  SurveyGridGeometry grid;
  grid.inlineMin = g.inlineMin; grid.inlineMax = g.inlineMax;
  grid.xlineMin = g.xlineMin; grid.xlineMax = g.xlineMax;
  grid.p1Inline = g.inlineMin; grid.p1Xline = g.xlineMin;
  grid.p1x = g.cornerX[0]; grid.p1y = g.cornerY[0];
  grid.a = (g.cornerX[1] - g.cornerX[0]) / dx;
  grid.c = (g.cornerY[1] - g.cornerY[0]) / dx;
  grid.b = (g.cornerX[3] - g.cornerX[0]) / di;
  grid.d = (g.cornerY[3] - g.cornerY[0]) / di;
  grid.valid = std::isfinite(grid.a * grid.d - grid.b * grid.c) &&
               std::abs(grid.a * grid.d - grid.b * grid.c) > 1e-12;
  if (!grid.valid) {
    result.error = QStringLiteral("地震体坐标无法定位层位反射窗");
    return result;
  }
  if (source == SeismicHorizonSource::TimeRaster) {
    const auto samples = lines.take(0);
    for (const auto &sample : samples) {
      const int row = sample.cell / result.columns, col = sample.cell % result.columns;
      const double x = result.extent.left() + (col + 0.5) * result.extent.width() / result.columns;
      const double y = result.extent.bottom() - (row + 0.5) * result.extent.height() / result.rows;
      double inlinePosition = 0, xlinePosition = 0;
      grid.xyToInlineXlineContinuous(x, y, &inlinePosition, &xlinePosition);
      if (!std::isfinite(inlinePosition) || !std::isfinite(xlinePosition) ||
          inlinePosition < g.inlineMin || inlinePosition > g.inlineMax ||
          xlinePosition < g.xlineMin || xlinePosition > g.xlineMax)
        continue;
      int il = 0, xl = 0;
      if (grid.xyToInlineXline(x, y, &il, &xl))
        lines[il].append({sample.cell, xl, sample.timeMs});
    }
  } else {
    QFile input(horizonPath);
    if (!input.open(QIODevice::ReadOnly)) {
      result.error = QStringLiteral("无法读取层位文件：%1").arg(input.errorString());
      return result;
    }
    QByteArray header;
    while (!input.atEnd()) {
      const qint64 position = input.pos();
      const auto line = input.readLine();
      if (!line.trimmed().startsWith('#') && !line.trimmed().isEmpty()) {
        input.seek(position);
        break;
      }
      header += line;
      if (header.size() > 65536) {
        result.error = QStringLiteral("层位文件头过长，无法识别 SMI 格式");
        return result;
      }
    }
    HorizonHeader h;
    if (!parseHorizonHeader(header, &h, &result.error)) return result;
    if (h.zUnits.compare(QLatin1String("ms"), Qt::CaseInsensitive) != 0) {
      result.error = QStringLiteral("直接提取需要毫秒（ms）单位的层位时间");
      return result;
    }
    const auto horizonGrid = SurveyGridGeometry::fromHorizonHeader(h);
    const double spacingX = (h.p2x - h.p1x) / (h.gridCols - 1);
    const double spacingY = (h.p3y - h.p2y) / (h.gridRows - 1);
    // 号域相同仍须核对坐标，避免把另一测区的同号道当成本测区。
    for (const auto &corner : std::array<QPair<int, int>, 3>{
           QPair<int, int>{h.p1Inline, h.p1Xline},
           {h.p2Inline, h.p2Xline}, {h.p3Inline, h.p3Xline}}) {
      double sx = 0, sy = 0, hx = 0, hy = 0;
      grid.inlineXlineToXy(corner.first, corner.second, &sx, &sy);
      horizonGrid.inlineXlineToXy(corner.first, corner.second, &hx, &hy);
      if (std::hypot(sx - hx, sy - hy) > 1.0) {
        result.error = QStringLiteral("层位坐标与所选地震测网不一致，请核对测区");
        return result;
      }
    }
    result.columns = h.gridCols;
    result.rows = h.gridRows;
    // 原始点落在输出像元中心；这里只定义分布图网格，不生成中间时间栅格。
    result.extent = QRectF(h.p1x - spacingX / 2, h.p1y - spacingY / 2,
                          h.gridCols * spacingX, h.gridRows * spacingY);
    QByteArray occupied(result.rows * result.columns, '\0');
    const QRegularExpression spaces(QStringLiteral("\\s+"));
    int readRows = 0;
    while (!input.atEnd()) {
      if ((++readRows % 1024) == 0) {
        if (stopped()) { result.error = QStringLiteral("已取消"); return result; }
        report(20 + int(10.0 * input.pos() / std::max<qint64>(1, input.size())));
      }
      const auto text = QString::fromUtf8(input.readLine()).trimmed();
      if (text.isEmpty() || text.startsWith('#')) continue;
      const auto fields = text.split(spaces, Qt::SkipEmptyParts);
      if (fields.size() < 5) continue;
      bool xOk, yOk, timeOk, ilOk, xlOk;
      const double x = fields[0].toDouble(&xOk), y = fields[1].toDouble(&yOk);
      const double time = fields[2].toDouble(&timeOk);
      const int il = fields[3].toInt(&ilOk), xl = fields[4].toInt(&xlOk);
      if (!xOk || !yOk || !timeOk || !ilOk || !xlOk ||
          !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(time) || time == -9999 ||
          !horizonGrid.inlineXlineInside(il, xl) || !grid.inlineXlineInside(il, xl)) continue;
      double expectedX = 0, expectedY = 0;
      horizonGrid.inlineXlineToXy(il, xl, &expectedX, &expectedY);
      if (std::abs(x - expectedX) > 0.001 || std::abs(y - expectedY) > 0.001) continue;
      const int cell = (h.p1Inline + h.gridRows - 1 - il) * result.columns + xl - h.p1Xline;
      if (occupied[cell]) {
        result.error = QStringLiteral("层位文件有重复的 Inline/Crossline 位置，请先处理重复点");
        return result;
      }
      occupied[cell] = 1;
      lines[il].append({cell, xl, time});
    }
    if (input.error() != QFileDevice::NoError) {
      result.error = QStringLiteral("读取层位文件失败：%1").arg(input.errorString());
      return result;
    }
  }
  const int count = result.columns * result.rows;
  using Feature = std::array<double, 3>;
  QVector<Feature> features;
  QVector<int> cells;
  int sampled = 0;
  int lineIndex = 0;
  for (auto it = lines.cbegin(); it != lines.cend(); ++it) {
    if (stopped()) { result.error = QStringLiteral("已取消"); return result; }
    QVector<SegyTrace> traces;
    QString error;
    if (!reader.readInline(it.key(), &traces, &error, &options))
      continue;
    QMap<int, const SegyTrace *> byXline;
    for (const auto &trace : traces) byXline[trace.xlineNo] = &trace;
    for (const auto &entry : it.value()) {
      if ((++sampled % 256) == 0 && stopped()) {
        result.error = QStringLiteral("已取消"); return result;
      }
      const auto *trace = byXline.value(entry.xline, nullptr);
      if (!trace) continue;
      const double dt = (trace->sampleIntervalUs > 0 ? trace->sampleIntervalUs : reader.sampleIntervalUs()) / 1000.0;
      const double t0 = std::isfinite(trace->startTimeMs) ? trace->startTimeMs : g.startTimeMs;
      if (!(dt > 0) || !std::isfinite(dt)) continue;
      const double start = (entry.timeMs - 12.0 - t0) / dt;
      const double end = (entry.timeMs + 12.0 - t0) / dt;
      if (!std::isfinite(start) || !std::isfinite(end) || start < 0 ||
          end > trace->samples.size() - 1) continue;
      const int first = int(std::ceil(start)), last = int(std::floor(end));
      if (last <= first) continue;
      double mean = 0, rms = 0, changes = 0;
      bool finite = true;
      for (int sample = first; sample <= last; ++sample) {
        const double value = trace->samples[sample];
        if (!std::isfinite(value)) { finite = false; break; }
        mean += value; rms += value * value;
        if (sample > first && value * trace->samples[sample - 1] < 0) ++changes;
      }
      if (!finite) continue;
      const double n = last - first + 1;
      features.append({mean / n, std::sqrt(rms / n), changes / (n - 1)});
      cells.append(entry.cell);
    }
    report(30 + 45 * ++lineIndex / std::max(1, int(lines.size())));
  }
  if (stopped()) { result.error = QStringLiteral("已取消"); return result; }
  if (features.size() < codes.size()) {
    result.error = QStringLiteral("层位与地震反射窗的有效交集不足，无法聚类");
    return result;
  }
  // 标准化均值、RMS、过零率，避免振幅量纲支配聚类。
  Feature mean{}, scale{};
  for (const auto &feature : features)
    for (int d = 0; d < 3; ++d) mean[d] += feature[d] / features.size();
  for (const auto &feature : features)
    for (int d = 0; d < 3; ++d) scale[d] += std::pow(feature[d] - mean[d], 2) / features.size();
  for (auto &feature : features)
    for (int d = 0; d < 3; ++d) feature[d] = (feature[d] - mean[d]) / std::max(1e-12, std::sqrt(scale[d]));
  const auto distance = [](const Feature &a, const Feature &b) {
    double sum = 0; for (int d = 0; d < 3; ++d) sum += std::pow(a[d] - b[d], 2); return sum;
  };
  QVector<Feature> centers{features.first()};
  while (centers.size() < codes.size()) {
    double farthest = -1; int chosen = 0;
    for (int i = 0; i < features.size(); ++i) {
      if ((i % 1024) == 0 && stopped()) {
        result.error = QStringLiteral("已取消"); return result;
      }
      double nearest = std::numeric_limits<double>::infinity();
      for (const auto &center : centers) nearest = std::min(nearest, distance(features[i], center));
      if (nearest > farthest) { farthest = nearest; chosen = i; }
    }
    centers.append(features[chosen]);
  }
  QVector<int> labels(features.size(), -1);
  for (int iteration = 0; iteration < 40; ++iteration) {
    if (stopped()) { result.error = QStringLiteral("已取消"); return result; }
    QVector<Feature> sums(codes.size()); QVector<int> sizes(codes.size(), 0);
    bool changed = false;
    for (int i = 0; i < features.size(); ++i) {
      if ((i % 1024) == 0 && stopped()) {
        result.error = QStringLiteral("已取消"); return result;
      }
      double nearest = std::numeric_limits<double>::infinity(); int label = 0;
      for (int k = 0; k < centers.size(); ++k) {
        const double dist = distance(features[i], centers[k]);
        if (dist < nearest) { nearest = dist; label = k; }
      }
      changed = changed || labels[i] != label; labels[i] = label; ++sizes[label];
      for (int d = 0; d < 3; ++d) sums[label][d] += features[i][d];
    }
    for (int k = 0; k < centers.size(); ++k)
      if (sizes[k]) for (int d = 0; d < 3; ++d) centers[k][d] = sums[k][d] / sizes[k];
    report(75 + iteration * 24 / 40);
    if (!changed) break;
  }
  QVector<int> order(codes.size()); std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return centers[a][1] < centers[b][1]; });
  QVector<int> classCodes(codes.size());
  for (int k = 0; k < order.size(); ++k) classCodes[order[k]] = codes[k];
  result.cells.fill(-9999, count);
  for (int i = 0; i < cells.size(); ++i) result.cells[cells[i]] = classCodes[labels[i]];
  result.validCells = cells.size();
  report(100);
  return result;
}
