// 层：功能
#include "propertymodelworkflow.h"

#include "../catalog/datacatalog.h"
#include "derivedassets.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVariantMap>

#include <gdal.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{

void setError(QString *error, const QString &text)
{
  if (error)
    *error = text;
}

QString num(double v) { return QString::number(v, 'g', 17); }

void appendFloatHash(QCryptographicHash *hash, const std::vector<float> &values)
{
  if (!values.empty())
    hash->addData(reinterpret_cast<const char *>(values.data()),
                  static_cast<int>(values.size() * sizeof(float)));
}

} // namespace

PropertyModelWorkflow::PropertyModelWorkflow(DataCatalog *catalog, const QString &projectDir,
                                             QObject *parent)
  : QObject(parent)
  , m_catalog(catalog)
  , m_projectDir(projectDir)
{
}

void PropertyModelWorkflow::rebind(DataCatalog *catalog, const QString &projectDir)
{
  m_catalog = catalog;
  m_projectDir = projectDir;
}

bool PropertyModelWorkflow::loadSurface(const QString &path, paleo::stratgrid::SurfaceGrid *out,
                                        QString *error)
{
  if (!out)
  {
    setError(error, QStringLiteral("层位输出为空"));
    return false;
  }
  *out = paleo::stratgrid::SurfaceGrid{};
  GDALAllRegister();
  GDALDatasetH ds = GDALOpen(path.toUtf8().constData(), GA_ReadOnly);
  if (!ds)
  {
    setError(error, QStringLiteral("读不了层位栅格 %1").arg(path));
    return false;
  }
  const int cols = GDALGetRasterXSize(ds);
  const int rows = GDALGetRasterYSize(ds);
  double gt[6] = {0, 0, 0, 0, 0, 0};
  GDALGetGeoTransform(ds, gt);
  if (GDALGetRasterCount(ds) < 1 || cols < 1 || rows < 1)
  {
    GDALClose(ds);
    setError(error, QStringLiteral("层位栅格 %1 没有波段").arg(path));
    return false;
  }
  const double skew = std::max(std::fabs(gt[2]), std::fabs(gt[4]));
  const double scale = 1.0 + std::fabs(gt[1]) + std::fabs(gt[5]);
  if (skew > 1e-6 * scale)
  {
    GDALClose(ds);
    setError(error, QStringLiteral("旋转或错切栅格本轮不支持：%1").arg(path));
    return false;
  }
  if (!(gt[1] > 0.0) || gt[5] == 0.0)
  {
    GDALClose(ds);
    setError(error, QStringLiteral("层位像元尺寸非法：%1").arg(path));
    return false;
  }
  GDALRasterBandH band = GDALGetRasterBand(ds, 1);
  int flag = 0;
  const double nodata = GDALGetRasterNoDataValue(band, &flag);
  std::vector<float> z(static_cast<std::size_t>(cols * rows));
  const CPLErr rc = GDALRasterIO(band, GF_Read, 0, 0, cols, rows, z.data(), cols, rows,
                                  GDT_Float32, 0, 0);
  GDALClose(ds);
  if (rc != CE_None)
  {
    setError(error, QStringLiteral("层位栅格读取失败：%1").arg(path));
    return false;
  }
  if (flag)
  {
    const float nd = static_cast<float>(nodata);
    for (float &v : z)
      if (!std::isfinite(v) || v == nd)
        v = std::numeric_limits<float>::quiet_NaN();
  }
  else
  {
    for (float &v : z)
      if (!std::isfinite(v))
        v = std::numeric_limits<float>::quiet_NaN();
  }
  out->cols = cols;
  out->rows = rows;
  out->originX = gt[0];
  out->originY = gt[3];
  out->dx = gt[1];
  out->dy = gt[5];
  out->z = std::move(z);
  return true;
}

std::vector<paleo::stratgrid::FaultSegment>
PropertyModelWorkflow::segmentsFromWkt(const QString &wkt)
{
  std::vector<paleo::stratgrid::FaultSegment> out;
  if (wkt.trimmed().isEmpty())
    return out;
  const bool closeRing = wkt.contains(QLatin1String("POLYGON"), Qt::CaseInsensitive);
  const QStringList chunks = wkt.split(QLatin1Char('('));
  static const QRegularExpression numRe(
      QStringLiteral("[-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?"));
  for (const QString &chunk : chunks)
  {
    std::vector<std::pair<double, double>> pts;
    QRegularExpressionMatchIterator it = numRe.globalMatch(chunk);
    std::vector<double> nums;
    while (it.hasNext())
      nums.push_back(it.next().captured().toDouble());
    for (std::size_t i = 0; i + 1 < nums.size(); i += 2)
      pts.emplace_back(nums[i], nums[i + 1]);
    if (pts.size() < 2)
      continue;
    if (closeRing)
    {
      const auto &a = pts.front();
      const auto &b = pts.back();
      if (std::fabs(a.first - b.first) > 1e-9 || std::fabs(a.second - b.second) > 1e-9)
        pts.push_back(a);
    }
    for (std::size_t i = 0; i + 1 < pts.size(); ++i)
    {
      paleo::stratgrid::FaultSegment seg;
      seg.x0 = pts[i].first;
      seg.y0 = pts[i].second;
      seg.x1 = pts[i + 1].first;
      seg.y1 = pts[i + 1].second;
      if (seg.x0 != seg.x1 || seg.y0 != seg.y1)
        out.push_back(seg);
    }
  }
  return out;
}

std::vector<paleo::stratgrid::FaultSegment>
PropertyModelWorkflow::segmentsFromFaultSet(const paleo::fault::FaultSet &faults)
{
  std::vector<paleo::stratgrid::FaultSegment> out;
  for (const paleo::fault::Fault &fault : faults.faults())
  {
    for (const paleo::fault::FaultHorizonCut &cut : fault.cuts)
    {
      const auto segs = segmentsFromWkt(cut.wkt);
      out.insert(out.end(), segs.begin(), segs.end());
    }
  }
  return out;
}

QString PropertyModelWorkflow::paramHash(const PropertyModelRequest &request,
                                         const paleo::stratgrid::SurfaceGrid &top,
                                         const paleo::stratgrid::SurfaceGrid &bot)
{
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData("paleo-propmodel-v1\n");
  hash.addData(request.propertyName.toUtf8());
  hash.addData("\n");
  hash.addData(request.topName.toUtf8());
  hash.addData("\n");
  hash.addData(request.botName.toUtf8());
  hash.addData("\n");
  hash.addData(QByteArray::number(request.nLayers));
  hash.addData("\n");
  hash.addData(paleo::stratgrid::aggregatorId(request.aggregator).toUtf8());
  hash.addData("\n");
  hash.addData(num(request.idwPower).toUtf8());
  hash.addData("\n");
  const auto addGrid = [&](const paleo::stratgrid::SurfaceGrid &g) {
    hash.addData(QByteArray::number(g.cols));
    hash.addData(",");
    hash.addData(QByteArray::number(g.rows));
    hash.addData(",");
    hash.addData(num(g.originX).toUtf8());
    hash.addData(",");
    hash.addData(num(g.originY).toUtf8());
    hash.addData(",");
    hash.addData(num(g.dx).toUtf8());
    hash.addData(",");
    hash.addData(num(g.dy).toUtf8());
    hash.addData("\n");
    appendFloatHash(&hash, g.z);
  };
  addGrid(top);
  addGrid(bot);

  std::vector<paleo::stratgrid::WellCurve> wells = request.wells;
  std::sort(wells.begin(), wells.end(),
            [](const paleo::stratgrid::WellCurve &a, const paleo::stratgrid::WellCurve &b) {
              return a.wellId < b.wellId;
            });
  for (const paleo::stratgrid::WellCurve &well : wells)
  {
    hash.addData(well.wellId.toUtf8());
    hash.addData("|");
    hash.addData(well.curveName.toUtf8());
    hash.addData("|");
    for (const paleo::stratgrid::WellStation &s : well.stations)
    {
      hash.addData(num(s.md).toUtf8());
      hash.addData(",");
      hash.addData(num(s.x).toUtf8());
      hash.addData(",");
      hash.addData(num(s.y).toUtf8());
      hash.addData(",");
      hash.addData(num(s.z).toUtf8());
      hash.addData(";");
    }
    hash.addData("|");
    for (const paleo::stratgrid::CurvePoint &p : well.curve)
    {
      hash.addData(num(p.md).toUtf8());
      hash.addData(",");
      hash.addData(num(p.value).toUtf8());
      hash.addData(";");
    }
    hash.addData("\n");
  }
  std::vector<paleo::stratgrid::FaultSegment> faults = request.faults;
  std::sort(faults.begin(), faults.end(),
            [](const paleo::stratgrid::FaultSegment &a, const paleo::stratgrid::FaultSegment &b) {
              if (a.x0 != b.x0)
                return a.x0 < b.x0;
              if (a.y0 != b.y0)
                return a.y0 < b.y0;
              if (a.x1 != b.x1)
                return a.x1 < b.x1;
              return a.y1 < b.y1;
            });
  for (const paleo::stratgrid::FaultSegment &f : faults)
  {
    hash.addData(num(f.x0).toUtf8());
    hash.addData(",");
    hash.addData(num(f.y0).toUtf8());
    hash.addData(",");
    hash.addData(num(f.x1).toUtf8());
    hash.addData(",");
    hash.addData(num(f.y1).toUtf8());
    hash.addData(";");
  }
  return QString::fromLatin1(hash.result().toHex());
}

PropertyModelOutput
PropertyModelWorkflow::run(const PropertyModelRequest &request,
                           const std::function<bool(double, const QString &)> &progress)
{
  PropertyModelOutput result;
  const auto fail = [&](const QString &why) {
    result.ok = false;
    result.error = why;
    emit modelFailed(why);
    return result;
  };
  const auto report = [&](double fraction, const QString &stage) {
    if (progress && !progress(fraction, stage))
    {
      result.error = QStringLiteral("已取消");
      return false;
    }
    return true;
  };

  if (!m_catalog || !m_catalog->isOpen())
    return fail(QStringLiteral("属性建模未绑定 catalog"));
  if (request.nLayers < 1)
    return fail(QStringLiteral("层数须 ≥ 1"));
  if (!(request.idwPower > 0.0))
    return fail(QStringLiteral("IDW 幂次须为正"));

  if (!report(0.02, QStringLiteral("读层位")))
    return fail(result.error);

  paleo::stratgrid::SurfaceGrid top = request.top;
  paleo::stratgrid::SurfaceGrid bot = request.bot;
  if (!request.useEmbeddedSurfaces)
  {
    QString err;
    if (!loadSurface(request.topPath, &top, &err))
      return fail(err);
    if (!loadSurface(request.botPath, &bot, &err))
      return fail(err);
  }
  else if (top.z.empty() || bot.z.empty())
  {
    return fail(QStringLiteral("嵌入层位面为空"));
  }

  if (!report(0.10, QStringLiteral("建格架")))
    return fail(result.error);

  paleo::stratgrid::ZoneGrid grid;
  QString err;
  if (!paleo::stratgrid::buildZoneGrid(top, bot, request.nLayers, &grid, &err))
    return fail(err);

  if (!report(0.20, QStringLiteral("粗化")))
    return fail(result.error);

  paleo::stratgrid::UpscaleTable table;
  if (!paleo::stratgrid::upscaleWells(grid, request.wells, request.aggregator, &table, &err))
    return fail(err);
  const std::vector<paleo::stratgrid::Seed> seeds = paleo::stratgrid::seedsFromUpscale(table);

  paleo::stratgrid::PropertyVolume volume;
  const bool filled = paleo::stratgrid::fillIdw(
      grid, seeds, request.faults, request.idwPower, &volume,
      [&](double fraction) { return report(0.30 + 0.60 * fraction, QStringLiteral("充填")); }, &err);
  if (!filled)
    return fail(err.isEmpty() ? QStringLiteral("已取消") : err);

  if (!report(0.94, QStringLiteral("落盘")))
    return fail(result.error);

  const QString hash = paramHash(request, top, bot);
  QJsonObject prov;
  prov.insert(QStringLiteral("param_hash"), hash);
  prov.insert(QStringLiteral("property"), request.propertyName);
  prov.insert(QStringLiteral("top"), request.topName);
  prov.insert(QStringLiteral("bot"), request.botName);
  prov.insert(QStringLiteral("curve"),
              request.wells.empty() ? QString() : request.wells.front().curveName);
  prov.insert(QStringLiteral("aggregator"), paleo::stratgrid::aggregatorId(request.aggregator));
  prov.insert(QStringLiteral("n_layers"), request.nLayers);
  prov.insert(QStringLiteral("idw_power"), request.idwPower);
  prov.insert(QStringLiteral("n_wells"), static_cast<int>(request.wells.size()));
  prov.insert(QStringLiteral("n_fault_segments"), static_cast<int>(request.faults.size()));
  const QByteArray blob = paleo::stratgrid::writePropertyBlob(volume, prov);
  if (blob.isEmpty())
    return fail(QStringLiteral("属性体序列化失败"));

  QString fileStem = request.propertyName.trimmed();
  if (fileStem.isEmpty())
    fileStem = QStringLiteral("PROP");
  fileStem.replace(QLatin1Char('/'), QLatin1Char('_'));
  fileStem.replace(QLatin1Char('\\'), QLatin1Char('_'));
  const QString fileName = QStringLiteral("PROP_%1.pprop").arg(fileStem);
  const QString display = QStringLiteral("%1 %2-%3")
                              .arg(request.propertyName.isEmpty() ? QStringLiteral("PROP")
                                                                  : request.propertyName,
                                   request.topName, request.botName);

  DerivedAssetRegistrar registrar(m_catalog, m_projectDir);
  QString stageErr;
  const DerivedStaging st = registrar.stage(QStringLiteral("property_volume"), display, fileName,
                                            &stageErr);
  if (!st.isValid())
    return fail(stageErr);
  QFile out(st.absolutePath);
  if (!out.open(QIODevice::WriteOnly) || out.write(blob) != blob.size())
    return fail(QStringLiteral("属性体写入失败：%1").arg(st.absolutePath));
  out.close();

  QVariantMap extra;
  extra.insert(QStringLiteral("param_hash"), hash);
  extra.insert(QStringLiteral("property"), request.propertyName);
  extra.insert(QStringLiteral("top_name"), request.topName);
  extra.insert(QStringLiteral("bot_name"), request.botName);
  extra.insert(QStringLiteral("aggregator"), paleo::stratgrid::aggregatorId(request.aggregator));
  extra.insert(QStringLiteral("n_layers"), request.nLayers);
  extra.insert(QStringLiteral("idw_power"), request.idwPower);
  extra.insert(QStringLiteral("ni"), volume.grid.ni);
  extra.insert(QStringLiteral("nj"), volume.grid.nj);
  extra.insert(QStringLiteral("nk"), volume.grid.nk);
  extra.insert(QStringLiteral("live_columns"), volume.grid.liveColumns);
  extra.insert(QStringLiteral("filled_cells"), volume.filledCells);
  extra.insert(QStringLiteral("unfilled_live_cells"), volume.unfilledLiveCells);
  QStringList curves;
  for (const paleo::stratgrid::WellCurve &well : request.wells)
    if (!curves.contains(well.curveName) && !well.curveName.isEmpty())
      curves.append(well.curveName);
  extra.insert(QStringLiteral("curves"), curves.join(QLatin1Char(',')));

  const QStringList parents =
      registrar.parentVersionIdsFor(QStringList{request.topPath, request.botPath});
  QString commitErr;
  if (!registrar.commit(st, parents, QStringLiteral("propertymodel/build"), extra, &commitErr))
    return fail(commitErr);

  if (progress)
    progress(1.0, QStringLiteral("完成"));

  result.ok = true;
  result.path = st.absolutePath;
  result.assetId = st.assetId;
  result.versionId = st.versionId;
  result.paramHash = hash;
  result.liveColumns = volume.grid.liveColumns;
  result.filledCells = volume.filledCells;
  result.unfilledLiveCells = volume.unfilledLiveCells;
  result.volume = std::move(volume);
  emit modelStored(result.path);
  return result;
}
