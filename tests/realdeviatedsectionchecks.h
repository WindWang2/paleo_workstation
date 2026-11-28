#pragma once

#include "domain/wellnumeric.h"
#include "io/areamodel.h"
#include "services/projectdata.h"
#include "services/fspathutils.h"
#include "services/seismicdataaccess.h"
#include "ui/seismicsection/seismicsectiondockwidget.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScopeGuard>
#include <QtTest>
#include <algorithm>
#include <cmath>

namespace {

bool sectionPixelsDiffer(const QImage &a, const QImage &b, double x, double y, int radius) {
  const QRect area(qRound(x) - radius, qRound(y) - radius, 2 * radius + 1, 2 * radius + 1);
  if (!a.rect().contains(area)) return false;
  for (int py = area.top(); py <= area.bottom(); ++py)
    for (int px = area.left(); px <= area.right(); ++px)
      if (a.pixel(px, py) != b.pixel(px, py)) return true;
  return false;
}

// Independent of SectionWellProjector: use the source XY checkshots and the two
// displayed wellheads as a local ruler on this straight HY line. Grid snapping
// may introduce a small difference between the physical route and that ruler.
struct HySectionRuler {
  glm::dvec2 axis;
  double tracesPerMetre;
  double along(const seismic::SectionWellInfo &well, double x, double y) const {
    return glm::dot(axis, glm::dvec2(x - well.surfaceX, y - well.surfaceY));
  }
  double offset(const seismic::SectionWellInfo &well, double x, double y) const {
    return std::abs(axis.y * (x - well.surfaceX) - axis.x * (y - well.surfaceY));
  }
};

void measureHyWell(seismic::SeismicSectionCanvas &canvas, const seismic::SectionWellInfo &well,
                   const QVector<TdSample> &rows, const HySectionRuler &ruler, const QImage &all,
                   const QImage &noCurves, const QImage &noWells, QJsonObject &evidence) {
  QVERIFY(well.calibrated && !well.timeReference.isEmpty());
  QVERIFY(well.selectedForSection && well.isWithinBuffer);
  QVERIFY(rows.size() >= 100);
  const double head = canvas.traceToPixelX(well.tracePosition);
  int matches = 0, probes = 0, pathHits = 0, outsideProbes = 0, outsideClear = 0;
  double minX = INFINITY, maxX = -INFINITY, minM = INFINITY, maxM = -INFINITY;
  for (const auto &row : rows) {
    if (!std::isfinite(row.x) || !std::isfinite(row.y) || row.timeMs < 0) continue;
    const double offset = ruler.offset(well, row.x, row.y);
    if (offset > canvas.bufferDistanceM() - 10) {
      const auto trace = well.tracePosition + ruler.tracesPerMetre * ruler.along(well, row.x, row.y);
      const auto x = canvas.traceToPixelX(trace), y = canvas.timeToPixelY(row.timeMs + well.timeShiftMs);
      if (offset > canvas.bufferDistanceM() + 40 && trace > 3 && trace < canvas.traceCount() - 3 &&
          noWells.rect().adjusted(4, 4, -4, -4).contains(qRound(x), qRound(y))) {
        ++outsideProbes;
        outsideClear += !sectionPixelsDiffer(noWells, noCurves, x, y, 2);
      }
      continue;
    }
    const auto it = std::lower_bound(well.trajectory.begin(), well.trajectory.end(), row.md,
                                     [](const auto &point, double md) { return point.md < md; });
    QVERIFY(it != well.trajectory.end());
    QVERIFY(std::abs(it->md - row.md) < 1e-6);
    QVERIFY(std::abs(it->twtMs - row.timeMs - well.timeShiftMs) < 1e-5);
    const double along = ruler.along(well, row.x, row.y);
    const double trace = well.tracePosition + ruler.tracesPerMetre * along;
    QVERIFY2(std::isfinite(it->tracePosition) && std::abs(it->tracePosition - trace) < 1.5,
             qPrintable(QString("%1 MD %2 expected trace %3, got %4").arg(well.wellName).arg(row.md).arg(trace).arg(it->tracePosition)));
    const double x = canvas.traceToPixelX(it->tracePosition);
    const double y = canvas.timeToPixelY(row.timeMs + well.timeShiftMs);
    minX = std::min(minX, x); maxX = std::max(maxX, x);
    minM = std::min(minM, along); maxM = std::max(maxM, along);
    ++matches;
    if (std::abs(x - head) > 8 && noCurves.rect().adjusted(4, 4, -4, -4).contains(qRound(x), qRound(y))) {
      ++probes;
      pathHits += sectionPixelsDiffer(noCurves, noWells, x, y, 2);
    }
  }
  QVERIFY(matches >= 100);
  QVERIFY(maxM - minM > 45);
  QVERIFY(maxX - minX > 10);
  QVERIFY(probes >= 10 && pathHits > 0.8 * probes);

  // At real LAS MD interpolate the source XYZ, then look for pixels changed
  // only by the GR toggle at the displaced bore and the calibrated sample time.
  const seismic::WellCurveItem *gr = nullptr;
  size_t most = 0;
  for (const auto &curve : well.curves) {
    if (seismic::baseCurveMnemonic(curve.curveName).compare("GR", Qt::CaseInsensitive)) continue;
    size_t valid = 0;
    for (size_t i = 0; i < std::min(curve.twtMs.size(), curve.values.size()); ++i)
      if (std::isfinite(curve.twtMs[i]) && !paleo::wellnumeric::isNullCurveValue(curve.values[i])) ++valid;
    if (valid > most) { most = valid; gr = &curve; }
  }
  int curveProbes = 0, curveHits = 0, formerlySwitched = 0, fixedSideHits = 0;
  if (gr) {
    QCOMPARE(gr->depthsM.size(), gr->values.size());
    QCOMPARE(gr->twtMs.size(), gr->values.size());
    const double right = canvas.traceToPixelX(canvas.traceCount() - 1);
    // In the fitted two-well overview the end wells' GR tracks face inward
    // for the entire path, even where the right-hand trajectory leaves the edge.
    const bool trackOnLeft = head > canvas.traceToPixelX((canvas.traceCount() - 1) / 2.);
    for (size_t i = 0; i < gr->values.size(); i += std::max<size_t>(1, gr->values.size() / 1000)) {
      const auto md = gr->depthsM[i];
      if (!std::isfinite(md) || !std::isfinite(gr->twtMs[i]) || paleo::wellnumeric::isNullCurveValue(gr->values[i])) continue;
      const auto hi = std::upper_bound(rows.cbegin(), rows.cend(), md, [](double d, const TdSample &row) { return d < row.md; });
      if (hi == rows.cbegin() || hi == rows.cend()) continue;
      const auto &lo = *(hi - 1);
      if (hi->md <= lo.md || lo.timeMs < 0) continue;
      const double f = (md - lo.md) / (hi->md - lo.md);
      const double x = std::lerp(lo.x, hi->x, f), y = std::lerp(lo.y, hi->y, f);
      if (!std::isfinite(x) || !std::isfinite(y) || ruler.offset(well, x, y) > canvas.bufferDistanceM() - 10) continue;
      const double bore = canvas.traceToPixelX(well.tracePosition + ruler.tracesPerMetre * ruler.along(well, x, y));
      if (std::abs(bore - head) < 8) continue;
      const double time = std::lerp(lo.timeMs, hi->timeMs, f) + well.timeShiftMs;
      QVERIFY(std::abs(time - gr->twtMs[i]) < 1e-4);
      const double track = bore + (trackOnLeft ? -40 : 4) + 36 * std::clamp((gr->values[i] - 20.) / 130., 0., 1.);
      const double py = canvas.timeToPixelY(time);
      if (!all.rect().adjusted(5, 5, -5, -5).contains(qRound(track), qRound(py))) continue;
      ++curveProbes;
      const bool hit = sectionPixelsDiffer(all, noCurves, track, py, 4);
      curveHits += hit;
      if (trackOnLeft && bore + 48 < right) {
        ++formerlySwitched;
        fixedSideHits += hit;
      }
    }
  }
  evidence = {{"well", well.wellName}, {"reference", well.timeReference}, {"checkshotsInside", matches},
              {"projectedSpanM", maxM - minM}, {"drawnSpanPx", maxX - minX},
              {"trajectoryProbes", probes}, {"trajectoryHits", pathHits},
              {"outsideProbes", outsideProbes}, {"outsideClear", outsideClear},
              {"grProbes", curveProbes}, {"grHits", curveHits},
              {"grFormerlySwitchedProbes", formerlySwitched}, {"grFixedSideHits", fixedSideHits}};
  qInfo() << evidence;
  if (outsideProbes > 0) QVERIFY(outsideClear > 0.9 * outsideProbes);
  if (curveProbes > 0) QVERIFY(curveHits > 0.8 * curveProbes);
  if (formerlySwitched > 0) QVERIFY(fixedSideHits > 0.8 * formerlySwitched);
}

void verifyRealDeviatedSection(seismic::SeismicSectionDockWidget &dock, ProjectDataFacade &data,
                               const QStringList &ids, const QString &output) {
  auto &canvas = *dock.canvas();
  QVERIFY(canvas.traceCount() > 100 && canvas.sampleCount() > 100);
  QVERIFY(dock.volume());
  const auto path = paleo::fromFsPath(dock.volume()->Path());
  QString detail;
  std::string error;
  paleo::SeismicDataAccess source;
  QVERIFY2(source.open(path, &detail) && source.registered(), qPrintable(detail));
  paleo::data::SeismicFileInfo info;
  QVERIFY2(source.inspect(info, error), error.c_str());
  QCOMPARE(canvas.sampleCount(), info.sampleCount);
  QCOMPARE(canvas.startSampleMs(), info.firstTime);
  QCOMPARE(canvas.sampleIntervalMs(), info.sampleInterval);
  float low = INFINITY, high = -INFINITY;
  for (int i = 0; i < canvas.traceCount(); i += 19)
    for (int j = 0; j < canvas.sampleCount(); j += 67) {
      const float amplitude = canvas.displayValueAt(i, j);
      if (std::isfinite(amplitude)) { low = std::min(low, amplitude); high = std::max(high, amplitude); }
    }
  QVERIFY(low < high);
  QCOMPARE(canvas.verticalUnit(), seismic::SectionVerticalUnit::TwoWayTimeMs);
  QCOMPARE(canvas.verticalExaggeration(), 1.);
  QCOMPARE(canvas.bufferDistanceM(), 500.);
  QVERIFY(canvas.showWells() && canvas.showWellCurves());
  const QImage all = canvas.grabCanvasImage();
  if (!output.isEmpty()) QVERIFY(all.save(QDir(output).filePath("HY-well-seismic-section.png")));
  canvas.setShowWellCurves(false);
  const QImage noCurves = canvas.grabCanvasImage();
  canvas.setShowWells(false);
  const QImage noWells = canvas.grabCanvasImage();
  canvas.setShowWells(true);
  canvas.setShowWellCurves(true);
  std::vector<const seismic::SectionWellInfo *> ordered;
  for (const auto &id : ids) {
    const auto found = std::find_if(canvas.wells().begin(), canvas.wells().end(), [&](const auto &w) { return w.wellId == id; });
    QVERIFY(found != canvas.wells().end());
    ordered.push_back(&*found);
  }
  const glm::dvec2 delta(ordered[1]->surfaceX - ordered[0]->surfaceX, ordered[1]->surfaceY - ordered[0]->surfaceY);
  const double distance = glm::length(delta);
  QVERIFY(distance > 1000.);
  const HySectionRuler ruler{delta / distance, (ordered[1]->tracePosition - ordered[0]->tracePosition) / distance};
  QJsonArray wells;
  int curveProbes = 0, outsideProbes = 0, formerlySwitched = 0;
  for (const auto *well : ordered) {
    QJsonObject sample;
    measureHyWell(canvas, *well, data.tdTableFor(well->wellId), ruler, all, noCurves, noWells, sample);
    QVERIFY(!sample.isEmpty());
    wells.append(sample);
    curveProbes += sample["grProbes"].toInt();
    formerlySwitched += sample["grFormerlySwitchedProbes"].toInt();
    outsideProbes += sample["outsideProbes"].toInt();
  }
  QVERIFY(curveProbes >= 10);
  QVERIFY(formerlySwitched >= 10);
  QVERIFY(outsideProbes >= 10);
  if (!output.isEmpty()) {
    QSaveFile file(QDir(output).filePath("measurements.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QJsonObject report{{"source", path}, {"sourceBytes", double(info.fileBytes)},
                             {"routeLengthM", canvas.totalDistanceM()}, {"headSeparationM", distance},
                             {"columns", canvas.traceCount()}, {"seismicSamples", canvas.sampleCount()},
                             {"intervalMs", canvas.sampleIntervalMs()}, {"firstTimeMs", canvas.startSampleMs()},
                             {"bufferM", canvas.bufferDistanceM()}, {"wells", wells}};
    const auto json = QJsonDocument(report).toJson();
    QCOMPARE(file.write(json), json.size()); QVERIFY(file.commit());
  }
}
}
