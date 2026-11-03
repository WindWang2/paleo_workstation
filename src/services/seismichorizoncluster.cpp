// 层：数据
#include "seismichorizoncluster.h"
#include "seismicunitinput.h"
#include "stratigraphicsubintervals.h"
#include "seismicdataaccess.h"
#include "seismichorizonscatter.h"
#include "io/segyreader.h"
#include "services/seismicmapping.h"
#include "services/subintervalfactors.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <gdal.h>
#include <limits>
#include <memory>
#include <numeric>
#include <set>

SeismicClusterResult clusterSeismicHorizon(
    const QString &seismicPath, const QString &horizonPath,
    const QString &cacheDir, const QVector<int> &faciesCodes,
    const std::function<void(int)> &progress,
    const std::function<bool()> &cancel, SeismicHorizonSource source) {
  SeismicClusterResult result;
  const auto stopped = [&] { return cancel && cancel(); };
  const auto report = [&](int value) { if (progress) progress(value); };
  if (stopped()) {
    result.error = QStringLiteral("已取消");
    return result;
  }
  if (faciesCodes.isEmpty() || faciesCodes.size() > 64) {
    result.error = QStringLiteral("请配置 1–64 个相类别");
    return result;
  }
  SeismicHorizonDetail::Lines lines;
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
    const char *projRef = GDALGetProjectionRef(ds.get());
    if (projRef && *projRef)
      result.crsWkt = QString::fromUtf8(projRef);
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
        // nodata 按 float 口径归一（#165 同族）：times 是 float，非 float 可
        // 表示的 nodata（1e30/-99999.9 等）读入后与 double 恒不相等。
        if (std::isfinite(times[cell]) &&
            !(hasNoData && times[cell] == static_cast<float>(noData)))
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
  // finding-91：线号步长不能默认 1——步长 >1 的测网上，默认步长会把连续
  // 解 round 到不存在的线号（inline 侧整线样点被 readInline 丢弃、xline 侧
  // 样点静默丢弃，约一半栅格样点丢失）。从读取器索引的实际线号序列推最小
  // 间隔（同 gridPropagated 的 axisStep 口径）；推不出（<2 条线）保持 1。
  const auto axisStep = [](const QVector<qint32> &lines) {
    if (lines.size() < 2)
      return 1;
    int step = int(lines[1] - lines[0]);
    for (int i = 2; i < lines.size(); ++i)
      step = std::min(step, int(lines[i] - lines[i - 1]));
    return std::max(1, step);
  };
  grid.inlineStep = axisStep(reader.inlineNumbers());
  grid.xlineStep = axisStep(reader.crosslineNumbers());
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
  } else if (!SeismicHorizonDetail::readScatter(horizonPath, reader, &lines, &result, progress, cancel)) {
    return result;
  }

  const int count = result.columns * result.rows;
  using Feature = std::array<double, 3>;
  QVector<Feature> features;
  QVector<int> cells;
  int sampled = 0;
  int lineIndex = 0;
  int droppedInlines = 0;
  for (auto it = lines.cbegin(); it != lines.cend(); ++it) {
    if (stopped()) { result.error = QStringLiteral("已取消"); return result; }
    QVector<SegyTrace> traces;
    QString error;
    if (!reader.readInline(it.key(), &traces, &error, &options)) {
      ++droppedInlines;
      continue;
    }
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
  if (features.size() < faciesCodes.size()) {
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
  while (centers.size() < faciesCodes.size()) {
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
    QVector<Feature> sums(faciesCodes.size()); QVector<int> sizes(faciesCodes.size(), 0);
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
  QVector<int> order(faciesCodes.size()); std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return centers[a][1] < centers[b][1]; });
  QVector<int> classCodes(faciesCodes.size());
  for (int k = 0; k < order.size(); ++k) classCodes[order[k]] = faciesCodes[k];
  result.cells.fill(-9999, count);
  for (int i = 0; i < cells.size(); ++i) result.cells[cells[i]] = classCodes[labels[i]];
  result.validCells = cells.size();
  if (droppedInlines > 0)
    result.warnings.append(QStringLiteral("有 %1 条 inline 读取失败被跳过，相带覆盖缩水（详见日志）")
                               .arg(droppedInlines));
  report(100);
  return result;
}

SeismicClusterResult clusterSeismicStratigraphicWindow(
    const QString &projectDir, const paleo::strat::UnitSelection &unit, const QString &seismicPath,
    const QVector<int> &faciesCodes,
    const std::function<void(int)> &progress,
    const std::function<bool()> &cancel) {
  SeismicClusterResult result;
  const auto stopped = [&] { return cancel && cancel(); };
  const auto report = [&](int value) { if (progress) progress(value); };
  if (stopped()) {
    result.error = QStringLiteral("已取消");
    return result;
  }
  if (faciesCodes.isEmpty() || faciesCodes.size() > 64) {
    result.error = QStringLiteral("请配置 1–64 个相类别");
    return result;
  }
  report(5);
  paleo::data::SeismicHorizonWindowResult win;paleo::strat::SeismicAnalysisSource context;
  if(!paleo::strat::readSeismicAnalysisWindow(projectDir,unit,seismicPath,win,&result.error,cancel,&context))return result;
  result.crsWkt=context.crs;
  result.parameters={{"unitSelection",paleo::strat::toMap(context.unit)},{"seismic_volume_id",context.volumeId},
    {"seismic_source",context.sourcePath},{"sampled_trace_count",qulonglong(win.traces.size())},
    {"resampled_samples_per_trace",win.fixedSampleCount},{"input_attribute","layer_window_mean_rms_zero_crossings"},
    {"window_type","stratigraphic_unit"},{"horizon_source_format","dataservice_horizon_window"}};
  report(55);

  const int K = std::max(1, win.fixedSampleCount);
  double xmin = std::numeric_limits<double>::infinity(), xmax = -xmin;
  double ymin = xmin, ymax = -xmin;
  std::set<int> sampledInlines,sampledCrosslines;
  for (const auto &tr : win.traces) {
    sampledInlines.insert(tr.inlineNumber);sampledCrosslines.insert(tr.crosslineNumber);
    if (!std::isfinite(tr.x) || !std::isfinite(tr.y)) continue;
    xmin = std::min(xmin, tr.x); xmax = std::max(xmax, tr.x);
    ymin = std::min(ymin, tr.y); ymax = std::max(ymax, tr.y);
  }
  result.columns=std::min(64,int(sampledCrosslines.size()));
  result.rows=std::min(64,int(sampledInlines.size()));
  const double dx = (xmax - xmin) / std::max(1, result.columns - 1);
  const double dy = (ymax - ymin) / std::max(1, result.rows - 1);
  if (!std::isfinite(dx) || !std::isfinite(dy) || dx <= 0 || dy <= 0) {
    result.error = QStringLiteral("层位变时深窗与地震缺少有效的二维坐标交集");
    return result;
  }
  result.extent = QRectF(xmin - dx / 2.0, ymin - dy / 2.0, result.columns * dx, result.rows * dy);

  using Feature = std::array<double, 3>;
  struct CellPick {
    double distance = std::numeric_limits<double>::infinity();
    Feature feature{};
    bool valid = false;
  };
  const int count = result.columns * result.rows;
  QVector<CellPick> gridPicks(count);
  for (size_t idx = 0; idx < win.traces.size(); ++idx) {
    if ((idx % 512) == 0 && stopped()) {
      result.error = QStringLiteral("已取消");
      return result;
    }
    const auto &tr = win.traces[idx];
    if (!std::isfinite(tr.x) || !std::isfinite(tr.y)) continue;
    const int col = std::clamp(int(std::lround((tr.x-xmin)/dx)),0,result.columns-1);
    const int row = std::clamp(int(std::lround((ymax-tr.y)/dy)),0,result.rows-1),cell=row*result.columns+col;
    const double dist=std::hypot((tr.x-xmin)/dx-col,(ymax-tr.y)/dy-row);
    auto &pick=gridPicks[cell];
    if(dist>=pick.distance)continue;
    pick.distance=dist;pick.valid=false; // A nearer missing trace must not borrow a farther valid one.
    if(tr.status!=1)continue;
    const size_t baseOffset = idx * size_t(K);
    if (baseOffset + size_t(K) > win.amplitudes.size() || K < 2) continue;
    double mean = 0.0, rms = 0.0, changes = 0.0;
    bool finite = true;
    for (int k = 0; k < K; ++k) {
      const double v = win.amplitudes[baseOffset + size_t(k)];
      if (!std::isfinite(v)) { finite = false; break; }
      mean += v;
      rms += v * v;
      if (k > 0 && v * double(win.amplitudes[baseOffset + size_t(k - 1)]) < 0.0)
        ++changes;
    }
    if (!finite) continue;
    pick.valid=true;pick.feature={mean/K,std::sqrt(rms/K),changes/(K-1)};
  }

  QVector<Feature> features;
  QVector<int> cells;
  for (int c = 0; c < count; ++c) {
    if (gridPicks[c].valid) {
      features.append(gridPicks[c].feature);
      cells.append(c);
    }
  }
  if (features.size() < faciesCodes.size()) {
    result.error = QStringLiteral("层位变时深窗与地震反射的有效交集不足，无法聚类");
    return result;
  }
  Feature mean{}, scale{};
  for (const auto &f : features)
    for (int d = 0; d < 3; ++d) mean[d] += f[d] / features.size();
  for (const auto &f : features)
    for (int d = 0; d < 3; ++d) scale[d] += std::pow(f[d] - mean[d], 2) / features.size();
  for (auto &f : features)
    for (int d = 0; d < 3; ++d) f[d] = (f[d] - mean[d]) / std::max(1e-12, std::sqrt(scale[d]));
  const auto distance = [](const Feature &a, const Feature &b) {
    double sum = 0; for (int d = 0; d < 3; ++d) sum += std::pow(a[d] - b[d], 2); return sum;
  };
  QVector<Feature> centers{features.first()};
  while (centers.size() < faciesCodes.size()) {
    double farthest = -1; int chosen = 0;
    for (int i = 0; i < features.size(); ++i) {
      double nearest = std::numeric_limits<double>::infinity();
      for (const auto &center : centers) nearest = std::min(nearest, distance(features[i], center));
      if (nearest > farthest) { farthest = nearest; chosen = i; }
    }
    centers.append(features[chosen]);
  }
  QVector<int> labels(features.size(), -1);
  for (int iteration = 0; iteration < 40; ++iteration) {
    if (stopped()) { result.error = QStringLiteral("已取消"); return result; }
    QVector<Feature> sums(faciesCodes.size()); QVector<int> sizes(faciesCodes.size(), 0);
    bool changed = false;
    for (int i = 0; i < features.size(); ++i) {
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
  QVector<int> order(faciesCodes.size()); std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return centers[a][1] < centers[b][1]; });
  QVector<int> classCodes(faciesCodes.size());
  for (int k = 0; k < order.size(); ++k) classCodes[order[k]] = faciesCodes[k];
  result.cells.fill(-9999, count);
  for (int i = 0; i < cells.size(); ++i) result.cells[cells[i]] = classCodes[labels[i]];
  result.validCells = cells.size();
  report(100);
  return result;
}

SeismicClusterResult clusterSeismicStratigraphicWindow(const QString &projectDir,const QString &targetUnitOrHorizon,
    const QString &schemeId,const QString &revision,const QVector<int> &faciesCodes,
    const std::function<void(int)> &progress,const std::function<bool()> &cancel){
  SeismicClusterResult failure;paleo::data::StratigraphicScheme scheme;
  if(!paleo::strat::loadProjectSchemeFromDir(projectDir,schemeId,revision,scheme,nullptr,&failure.error))return failure;
  const auto *sub=paleo::data::framework::resolveSubInterval(scheme,targetUnitOrHorizon.toStdString());
  if(!sub){failure.error=QStringLiteral("请选择具体的地震子层段 S1–S4");return failure;}
  const auto unit=paleo::strat::makeSelection(scheme,QString::fromStdString(sub->interval.value),sub->index);
  const auto path=paleo::seismicProjectSource(projectDir,{},&failure.error);if(path.isEmpty())return failure;
  return clusterSeismicStratigraphicWindow(projectDir,unit,path,faciesCodes,progress,cancel);
}
