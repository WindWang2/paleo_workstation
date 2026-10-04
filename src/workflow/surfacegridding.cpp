// 层：功能
#include "surfacegridding.h"

#include "../algorithms/rasteralgebra.h"
#include "../algorithms/surfacevolumes.h"
#include "../catalog/datacatalog.h"
#include "../io/constraintstore.h"
#include "../io/horizonbinner.h"
#include "derivedassets.h"

#include <qgsgeometry.h>
#include <qgspointxy.h>

#include <gdal.h>
#include <cpl_conv.h>

#include <QDir>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QUuid>

#include <cmath>
#include <limits>
#include <vector>

namespace gridsolver = paleo::gridsolver;
namespace rasteralgebra = paleo::rasteralgebra;
namespace surfacevolumes = paleo::surfacevolumes;

// 面运算/等厚链的几何口径：两输入栅格须同网格（维度 + geotransform，
// 容差 = 像元尺寸的 1e-6，与 paleoalgorithms.cpp 的 sameGrid 同式）。
namespace
{

struct RasterGrid
{
  int cols = 0, rows = 0;
  double gt[6] = {0, 0, 0, 0, 0, 0};
  bool hasNodata = false;
  double nodata = 0;
};

bool readFloatRaster(const QString &path, RasterGrid *grid, std::vector<float> *out,
                     QString *error)
{
  GDALAllRegister();
  GDALDatasetH ds = GDALOpen(path.toUtf8().constData(), GA_ReadOnly);
  if (!ds)
  {
    if (error)
      *error = QStringLiteral("无法打开栅格 %1").arg(path);
    return false;
  }
  grid->cols = GDALGetRasterXSize(ds);
  grid->rows = GDALGetRasterYSize(ds);
  GDALGetGeoTransform(ds, grid->gt);
  GDALRasterBandH band = GDALGetRasterBand(ds, 1);
  int flag = 0;
  grid->nodata = GDALGetRasterNoDataValue(band, &flag);
  grid->hasNodata = flag != 0;
  const std::size_t n = std::size_t(grid->rows) * std::size_t(grid->cols);
  out->resize(n);
  const bool ok = GDALRasterIO(band, GF_Read, 0, 0, grid->cols, grid->rows, out->data(),
                               grid->cols, grid->rows, GDT_Float32, 0, 0) == CE_None;
  GDALClose(ds);
  if (!ok)
  {
    if (error)
      *error = QStringLiteral("读取栅格失败：%1").arg(path);
    return false;
  }
  // 落盘 nodata（-9999 族）→ 核内 NaN 语义。
  for (float &v : *out)
    if (std::isnan(v) || (grid->hasNodata && v == static_cast<float>(grid->nodata))) // #165
      v = std::numeric_limits<float>::quiet_NaN();
  return true;
}

bool sameGrid(const RasterGrid &a, const RasterGrid &b)
{
  if (a.cols != b.cols || a.rows != b.rows)
    return false;
  const double tol =
      std::max({std::fabs(a.gt[1]), std::fabs(a.gt[5]), 1.0}) * 1e-6;
  for (int i = 0; i < 6; ++i)
    if (std::fabs(a.gt[i] - b.gt[i]) > tol)
      return false;
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// SurfaceGriddingWorkflow
// ---------------------------------------------------------------------------

SurfaceGriddingWorkflow::Inspect SurfaceGriddingWorkflow::inspectHorizonText(
    const QByteArray &text, const QString &constraintGpkgPath)
{
  Inspect out;
  const HorizonScatter sc = parseHorizonScatter(text);
  if (sc.points.isEmpty())
  {
    out.error = QStringLiteral("层位散点为空（无数据行）");
    return out;
  }
  out.pointCount = int(sc.points.size());
  out.minX = out.minY = 1e300;
  out.maxX = out.maxY = -1e300;
  for (const HorizonScatterPoint &p : sc.points)
  {
    out.minX = std::min(out.minX, p.x);
    out.maxX = std::max(out.maxX, p.x);
    out.minY = std::min(out.minY, p.y);
    out.maxY = std::max(out.maxY, p.y);
  }
  HorizonHeader hh;
  if (parseHorizonHeader(text, &hh) && hh.hasP1 && hh.hasP2 && hh.gridCols > 1)
  {
    const double cell = (hh.p2x - hh.p1x) / (hh.gridCols - 1);
    if (cell > 0 && std::isfinite(cell))
    {
      out.hasHeaderCell = true;
      out.headerCellSize = cell;
    }
  }
  if (!constraintGpkgPath.isEmpty())
  {
    const ConstraintStore store(constraintGpkgPath);
    for (const QVariantMap &r : store.load())
      if (r.value(QStringLiteral("type")).toString().trimmed() ==
          QLatin1String("break_line"))
      {
        out.hasConstraints = true;
        break;
      }
  }
  out.ok = true;
  return out;
}

SurfaceGriddingWorkflow::SurfaceGriddingWorkflow(QgisLayerService *layers, QObject *parent)
    : QObject(parent), m_layers(layers)
{
}

void SurfaceGriddingWorkflow::setCatalog(DataCatalog *catalog, const QString &projectDir)
{
  m_catalog = catalog;
  m_projectDir = projectDir;
}

void SurfaceGriddingWorkflow::setBarrierSource(const QString &constraintGpkgPath)
{
  m_barrierGpkgPath = constraintGpkgPath;
}

QString SurfaceGriddingWorkflow::gridHorizonText(const QString &horizon,
                                                 const QByteArray &text, const Options &opt,
                                                 const std::function<bool()> &cancel,
                                                 const std::function<void(const QString &, int)> &stage,
                                                 Outcome *outcome)
{
  const auto fail = [this, &horizon](const QString &msg)
  {
    emit griddingFailed(horizon, msg);
    return msg;
  };
  if (!m_catalog || m_projectDir.isEmpty())
    return fail(QStringLiteral("网格化未绑定数据目录（catalog）——派生产物无法登记到工程"));
  if (opt.cellSize <= 0.0 || !std::isfinite(opt.cellSize))
    return fail(QStringLiteral("网格尺寸必须为正数"));
  if (stage)
    stage(QStringLiteral("parse"), 2);

  // ---- 散点解析 + 网格几何（守卫：1 亿像元预算，审计 #33 口径）------------
  const HorizonScatter scatter = parseHorizonScatter(text);
  if (scatter.points.isEmpty())
    return fail(QStringLiteral("层位 %1 没有可解析的散点（跳过行 %2）")
                    .arg(horizon)
                    .arg(scatter.skipped));
  double minX = std::numeric_limits<double>::infinity(), maxX = -minX;
  double minY = std::numeric_limits<double>::infinity(), maxY = -minY;
  for (const HorizonScatterPoint &p : scatter.points)
  {
    minX = std::min(minX, p.x);
    maxX = std::max(maxX, p.x);
    minY = std::min(minY, p.y);
    maxY = std::max(maxY, p.y);
  }
  QString geomErr;
  const gridsolver::GridGeometry geom =
      gridsolver::geometryForExtent(minX, maxX, minY, maxY, opt.cellSize, &geomErr);
  if (!geom.isValid())
    return fail(QStringLiteral("输出网格超限：%1").arg(geomErr));

  // ---- 屏障：ConstraintStore 的 break_line 栅格化（半格超采样步进）-------
  std::vector<std::uint8_t> barrier;
  if (opt.useBarriers && !m_barrierGpkgPath.isEmpty())
  {
    const QString maskErr = buildBarrierMask(horizon, geom, &barrier);
    if (!maskErr.isEmpty())
      return fail(maskErr);
  }
  if (stage)
    stage(QStringLiteral("build"), 5);

  // ---- 求解（多级级联最小曲率/张力样条）-----------------------------------
  std::vector<gridsolver::ScatterPoint> pts;
  pts.reserve(scatter.points.size());
  for (const HorizonScatterPoint &p : scatter.points)
    pts.push_back({p.x, p.y, p.z});
  gridsolver::GriddingParams gp;
  gp.tension = opt.tension;
  gp.maxSweeps = opt.maxSweeps;
  gridsolver::IterationControl control;
  control.cancelRequested = cancel;
  control.onSweep = [&stage](int sweep, int maxSweeps, double)
  {
    if (stage)
      stage(QStringLiteral("build"),
            5 + static_cast<int>(90.0 * sweep / std::max(1, maxSweeps)));
  };
  std::vector<float> z;
  gridsolver::GriddingStats stats;
  QString solveErr;
  if (!gridsolver::solveMinimumCurvature(pts, geom, gp,
                                         barrier.empty() ? nullptr : barrier.data(), &z,
                                         &stats, &solveErr, control))
    return fail(QStringLiteral("网格化求解失败：%1").arg(solveErr));
  if (stage)
    stage(QStringLiteral("publish"), 96);

  // ---- 质量面统计（距数据距离）+ 留一法 CV（可选）--------------------------
  Outcome o;
  o.rows = geom.rows;
  o.cols = geom.cols;
  o.sweeps = stats.sweeps;
  o.converged = stats.converged;
  o.finalDelta = stats.finalDelta;
  o.constrainedNodes = stats.constrainedNodes;
  o.collisions = stats.collisions;
  o.rejected = stats.rejected + scatter.skipped;
  o.barrierCells = 0;
  for (std::uint8_t m : barrier)
    o.barrierCells += m ? 1 : 0;
  const std::vector<float> dist = gridsolver::distanceToData(pts, geom);
  double distMax = 0, distSum = 0;
  long long distCount = 0;
  for (float d : dist)
  {
    if (std::isnan(d))
      continue;
    distMax = std::max(distMax, double(d));
    distSum += d;
    ++distCount;
  }
  o.distToDataMax = distMax;
  o.distToDataMean = distCount > 0 ? distSum / distCount : 0;
  if (opt.runCrossValidation)
  {
    gridsolver::CrossValidationResult cv;
    QString cvErr;
    if (gridsolver::crossValidateLeaveOneOut(pts, geom, gp, opt.cvPoints, false, &cv,
                                             &cvErr, control))
    {
      o.cvRms = cv.rms;
      o.cvFolds = cv.folds;
    }
    // CV 失败不阻断主产物（QC 面）——如实留 0 折。
  }

  // ---- 受管落盘 + DERIVED 版本登记 -----------------------------------------
  BinnedHorizon b;
  b.rows = geom.rows;
  b.cols = geom.cols;
  b.dx = geom.dx;
  b.dy = geom.dy;
  b.originX = geom.originX;
  b.originY = geom.originY;
  b.z.resize(int(z.size()));
  float zMin = std::numeric_limits<float>::max();
  float zMax = std::numeric_limits<float>::lowest();
  int filled = 0;
  for (int i = 0; i < int(z.size()); ++i)
  {
    const float v = std::isnan(z[std::size_t(i)]) ? float(-9999.0) : z[std::size_t(i)];
    b.z[i] = v;
    if (v != -9999.0f)
    {
      zMin = std::min(zMin, v);
      zMax = std::max(zMax, v);
      ++filled;
    }
  }
  b.filledCells = filled;
  b.zMin = zMin;
  b.zMax = zMax;
  b.collisions = stats.collisions;
  b.rejected = o.rejected;
  b.hasInlineRange = scatter.hasInlineRange;
  b.hasXlineRange = scatter.hasXlineRange;
  b.inlineMin = scatter.inlineMin;
  b.inlineMax = scatter.inlineMax;
  b.xlineMin = scatter.xlineMin;
  b.xlineMax = scatter.xlineMax;

  QVariantMap extra;
  extra.insert(QStringLiteral("algorithm"), QStringLiteral("min_curvature"));
  extra.insert(QStringLiteral("tension"), opt.tension);
  extra.insert(QStringLiteral("cellSize"), opt.cellSize);
  extra.insert(QStringLiteral("sweeps"), stats.sweeps);
  extra.insert(QStringLiteral("converged"), stats.converged);
  extra.insert(QStringLiteral("finalDelta"), stats.finalDelta);
  extra.insert(QStringLiteral("constrainedNodes"), stats.constrainedNodes);
  extra.insert(QStringLiteral("collisions"), stats.collisions);
  extra.insert(QStringLiteral("rejected"), o.rejected);
  extra.insert(QStringLiteral("barrierCells"), o.barrierCells);
  extra.insert(QStringLiteral("distToDataMax"), o.distToDataMax);
  extra.insert(QStringLiteral("distToDataMean"), o.distToDataMean);
  if (o.cvFolds > 0)
  {
    extra.insert(QStringLiteral("cvRms"), o.cvRms);
    extra.insert(QStringLiteral("cvFolds"), o.cvFolds);
  }
  o.layerId = QStringLiteral("gridded.%1").arg(horizon);
  o.title = QStringLiteral("%1 网格化面（最小曲率）").arg(horizon);

  // ---- 线程纪律（#122）：DataCatalog 是单线程写对象（threadViolation 守卫），
  // stage/commit 必须在其所属线程执行。同线程（同步调用/测试）直接登记；
  // 任务线程只把 GeoTIFF 写进 artifacts/staging 临时目录，再把登记排队回
  // catalog 所属线程（publishGridded 收进受管路径 + commit + rasterReady）。
  // 排队而非 BlockingQueued：GUI 线程若在等任务收尾也不会死锁；登记失败经
  // griddingFailed 回报（此时任务本身已返回成功，Outcome 不含 asset/version）。
  if (QThread::currentThread() == m_catalog->thread())
  {
    QString pubErr = publishGridded(horizon, b, QString(), extra, &o);
    if (!pubErr.isEmpty())
      return pubErr; // publishGridded 已 emit griddingFailed
    if (stage)
      stage(QStringLiteral("publish"), 100);
    if (outcome)
      *outcome = o;
    return QString();
  }

  const QString tmpDir = QDir(m_projectDir)
                             .filePath(QStringLiteral("artifacts/staging/grid-%1")
                                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
  if (!QDir().mkpath(tmpDir))
    return fail(QStringLiteral("无法创建网格化临时目录：%1").arg(tmpDir));
  const QString tmpTif = QDir(tmpDir).filePath(QStringLiteral("GRID_%1_MINCURV.tif").arg(horizon));
  QString writeErr;
  if (!writeHorizonGeoTiff(b, tmpTif, &writeErr))
  {
    QDir(tmpDir).removeRecursively();
    return fail(QStringLiteral("写出 GeoTIFF 失败：%1").arg(writeErr));
  }
  const QPointer<SurfaceGriddingWorkflow> self(this);
  const bool queued = QMetaObject::invokeMethod(
      m_catalog,
      [self, horizon, b, tmpTif, tmpDir, extra, o]() mutable
      {
        if (self)
          self->publishGridded(horizon, b, tmpTif, extra, &o);
        QDir(tmpDir).removeRecursively();
      },
      Qt::QueuedConnection);
  if (!queued)
  {
    QDir(tmpDir).removeRecursively();
    return fail(QStringLiteral("无法把版本登记排队回数据目录线程"));
  }
  if (stage)
    stage(QStringLiteral("publish"), 100);
  if (outcome)
    *outcome = o; // tifPath/assetId/versionId 由 owner 线程登记后经 rasterReady 回报
  return QString();
}

// owner 线程登记：tmpTif 为空时就地写受管路径，否则把临时产物收进受管路径。
QString SurfaceGriddingWorkflow::publishGridded(const QString &horizon, const BinnedHorizon &b,
                                                const QString &tmpTif, const QVariantMap &extra,
                                                Outcome *o)
{
  const auto fail = [this, &horizon](const QString &msg)
  {
    emit griddingFailed(horizon, msg);
    return msg;
  };
  if (!m_catalog)
    return fail(QStringLiteral("网格化未绑定数据目录（catalog）——派生产物无法登记到工程"));
  DerivedAssetRegistrar registrar(m_catalog, m_projectDir);
  const DerivedStaging st = registrar.stage(
      QStringLiteral("gridded_surface"),
      QStringLiteral("%1 网格化面（最小曲率）").arg(horizon),
      QStringLiteral("GRID_%1_MINCURV.tif").arg(horizon));
  if (!st.isValid())
    return fail(QStringLiteral("派生资产落位失败（%1 网格化面）").arg(horizon));
  QString commitErr;
  bool ok = false;
  // 父版本不伪造：散点文本源头资产在壳侧（本接口只见文本）——provenance
  // 走 sourceUri（surfacegridding/<层位>），与厚度井位层同口径。
  const QString sourceUri = QStringLiteral("surfacegridding/%1").arg(horizon);
  if (tmpTif.isEmpty())
  {
    QString writeErr;
    if (!writeHorizonGeoTiff(b, st.absolutePath, &writeErr))
      return fail(QStringLiteral("写出 GeoTIFF 失败：%1").arg(writeErr));
    ok = registrar.commit(st, {}, sourceUri, extra, &commitErr);
  }
  else
  {
    ok = registrar.commitExternal(st, tmpTif, {}, sourceUri, extra, &commitErr);
  }
  if (!ok)
    return fail(QStringLiteral("版本登记失败：%1").arg(commitErr));
  o->tifPath = st.absolutePath;
  o->assetId = st.assetId;
  o->versionId = st.versionId;
  // 声明走信号回 GUI 线程（manifest 写队列纪律）。
  emit rasterReady(o->layerId, o->tifPath, o->title, horizon);
  return QString();
}

QString SurfaceGriddingWorkflow::buildBarrierMask(const QString &horizon,
                                                  const gridsolver::GridGeometry &geom,
                                                  std::vector<std::uint8_t> *mask) const
{
  const ConstraintStore store(m_barrierGpkgPath);
  const QVector<QVariantMap> rows = store.load(horizon);
  if (rows.isEmpty())
    return QString();
  // 先收 break_line 折线（typed C1 词面；其它 type 对网格化无语义）。
  QVector<QVector<QgsPointXY>> lines;
  for (const QVariantMap &r : rows)
  {
    if (r.value(QStringLiteral("type")).toString().trimmed() !=
        QLatin1String("break_line"))
      continue;
    const QgsGeometry g = QgsGeometry::fromWkt(r.value(QStringLiteral("wkt")).toString());
    if (g.isNull() || g.isEmpty())
      continue;
    const QgsMultiPolylineXY mpl =
        g.isMultipart() ? g.asMultiPolyline() : QgsMultiPolylineXY{g.asPolyline()};
    for (const QgsPolylineXY &pl : mpl)
      if (pl.size() >= 2)
        lines.append(pl);
  }
  if (lines.isEmpty())
    return QString();
  mask->assign(std::size_t(geom.rows) * geom.cols, 0);
  const double cell = geom.dx;
  auto cellOf = [&](double x, double y, int *row, int *col)
  {
    const double cf = (x - geom.originX) / geom.dx;
    const double rf = (geom.originY - y) / geom.dy;
    *col = int(std::floor(cf));
    *row = int(std::floor(rf));
    return *col >= 0 && *col < geom.cols && *row >= 0 && *row < geom.rows;
  };
  for (const QVector<QgsPointXY> &pl : lines)
    for (int i = 1; i < pl.size(); ++i)
    {
      const QgsPointXY &a = pl[i - 1];
      const QgsPointXY &b = pl[i];
      const double len = a.distance(b);
      const int steps = std::max(1, int(std::ceil(len / (cell * 0.5))));
      for (int s = 0; s <= steps; ++s)
      {
        int row = 0, col = 0;
        if (cellOf(a.x() + (b.x() - a.x()) * s / steps,
                   a.y() + (b.y() - a.y()) * s / steps, &row, &col))
          (*mask)[std::size_t(row) * geom.cols + col] = 1;
      }
    }
  return QString();
}

QString SurfaceGriddingWorkflow::isopachBetweenRasters(const QString &topTif,
                                                       const QString &baseTif,
                                                       const QString &label,
                                                       VolumeReport *report, bool writeManaged,
                                                       Outcome *isopachOutcome)
{
  RasterGrid top, base;
  std::vector<float> zTop, zBase;
  QString err;
  if (!readFloatRaster(topTif, &top, &zTop, &err) ||
      !readFloatRaster(baseTif, &base, &zBase, &err))
    return err;
  if (!sameGrid(top, base))
    return QStringLiteral("两栅格网格不一致（维度/范围/像元尺寸须相同）");

  const std::size_t n = std::size_t(top.rows) * top.cols;
  std::vector<float> iso(n);
  rasteralgebra::applyBinary(zTop.data(), zBase.data(), n, rasteralgebra::BinaryOp::Subtract,
                             iso.data());

  surfacevolumes::VolumeReport vr;
  QString volErr;
  const std::vector<float> zero(n, 0.0f);
  if (!surfacevolumes::volumeBetween(iso.data(), zero.data(), top.cols, top.rows,
                                     std::fabs(top.gt[1]), std::fabs(top.gt[5]), &vr, &volErr))
    return QStringLiteral("体积积分失败：%1").arg(volErr);
  if (report)
  {
    report->volume = vr.volume;
    report->absVolume = vr.absVolume;
    report->area = vr.area;
    report->cellArea = vr.cellArea;
    report->minThickness = vr.minThickness;
    report->maxThickness = vr.maxThickness;
    report->meanThickness = vr.meanThickness;
    report->cells = vr.cells;
    report->nullCells = vr.nullCells;
    report->positiveCells = vr.positiveCells;
    report->negativeCells = vr.negativeCells;
  }

  if (writeManaged)
  {
    if (!m_catalog || m_projectDir.isEmpty())
      return QStringLiteral("面运算未绑定数据目录（catalog）——等厚栅格无法受管登记");
  }
  QString isopachPath;
  if (writeManaged)
  {
    DerivedAssetRegistrar registrar(m_catalog, m_projectDir);
    const DerivedStaging st = registrar.stage(
        QStringLiteral("isopach"), QStringLiteral("%1 等厚图").arg(label),
        QStringLiteral("ISOPACH_%1.tif").arg(label));
    if (!st.isValid())
      return QStringLiteral("等厚派生资产落位失败");
    isopachPath = st.absolutePath;
    BinnedHorizon b;
    b.rows = top.rows;
    b.cols = top.cols;
    b.dx = std::fabs(top.gt[1]);
    b.dy = std::fabs(top.gt[5]);
    b.originX = top.gt[0];
    b.originY = top.gt[3];
    b.z.resize(int(n));
    float zMin = std::numeric_limits<float>::max();
    float zMax = std::numeric_limits<float>::lowest();
    int filled = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
      const float v = std::isnan(iso[i]) ? -9999.0f : iso[i];
      b.z[int(i)] = v;
      if (v != -9999.0f)
      {
        zMin = std::min(zMin, v);
        zMax = std::max(zMax, v);
        ++filled;
      }
    }
    b.filledCells = filled;
    b.zMin = zMin;
    b.zMax = zMax;
    QString writeErr;
    if (!writeHorizonGeoTiff(b, isopachPath, &writeErr))
      return QStringLiteral("写出等厚栅格失败：%1").arg(writeErr);
    QVariantMap extra;
    extra.insert(QStringLiteral("algorithm"), QStringLiteral("raster_algebra_subtract"));
    extra.insert(QStringLiteral("volume"), vr.volume);
    extra.insert(QStringLiteral("area"), vr.area);
    extra.insert(QStringLiteral("cells"), qlonglong(vr.cells));
    extra.insert(QStringLiteral("nullCells"), qlonglong(vr.nullCells));
    extra.insert(QStringLiteral("negativeCells"), qlonglong(vr.negativeCells));
    QString commitErr;
    if (!registrar.commit(st, {}, QStringLiteral("surfacegridding/isopach/%1").arg(label),
                          extra, &commitErr))
      return QStringLiteral("等厚版本登记失败：%1").arg(commitErr);
    if (isopachOutcome)
    {
      isopachOutcome->tifPath = isopachPath;
      isopachOutcome->assetId = st.assetId;
      isopachOutcome->versionId = st.versionId;
      isopachOutcome->layerId = QStringLiteral("isopach.%1").arg(label);
      isopachOutcome->title = QStringLiteral("%1 等厚图").arg(label);
    }
    emit rasterReady(QStringLiteral("isopach.%1").arg(label), isopachPath,
                     QStringLiteral("%1 等厚图").arg(label), QString());
  }
  return QString();
}

QString SurfaceGriddingWorkflow::VolumeReport::csv() const
{
  QString out;
  out += QStringLiteral("metric,value\n");
  out += QStringLiteral("cells,%1\n").arg(cells);
  out += QStringLiteral("null_cells,%1\n").arg(nullCells);
  out += QStringLiteral("positive_cells,%1\n").arg(positiveCells);
  out += QStringLiteral("negative_cells,%1\n").arg(negativeCells);
  out += QStringLiteral("cell_area,%1\n").arg(cellArea, 0, 'g', 12);
  out += QStringLiteral("area,%1\n").arg(area, 0, 'g', 12);
  // 体积 = z 单位 × 面积单位（时间层位为 ms·m²，换算 m³ 需速度场）。
  out += QStringLiteral("volume_z_units_times_area,%1\n").arg(volume, 0, 'g', 12);
  out += QStringLiteral("abs_volume_z_units_times_area,%1\n").arg(absVolume, 0, 'g', 12);
  out += QStringLiteral("min_thickness,%1\n").arg(minThickness, 0, 'g', 12);
  out += QStringLiteral("max_thickness,%1\n").arg(maxThickness, 0, 'g', 12);
  out += QStringLiteral("mean_thickness,%1\n").arg(meanThickness, 0, 'g', 12);
  return out;
}
