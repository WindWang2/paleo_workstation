// 层：功能
#include "propertymodelworkflow.h"
#include "workflowerrors_internal.h"

#include "../catalog/datacatalog.h"
#include "../io/lasparser.h"
#include "../services/jobrunner.h"
#include "../services/projectdata.h"
#include "../services/welllogset.h"
#include "derivedassets.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QVariantMap>

#include <gdal.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{

using paleo::workflow_detail::setError;

QString num(double v) { return QString::number(v, 'g', 17); }

void appendFloatHash(QCryptographicHash *hash, const std::vector<float> &values)
{
  if (!values.empty())
    hash->addData(reinterpret_cast<const char *>(values.data()),
                  static_cast<int>(values.size() * sizeof(float)));
}

QString resolveSurfacePath(const QString &projectDir, const QString &path)
{
  if (path.isEmpty())
    return path;
  if (QDir::isAbsolutePath(path))
    return QDir::cleanPath(path);
  return QDir::cleanPath(QFileInfo(QDir(projectDir).filePath(path)).absoluteFilePath());
}

std::vector<paleo::stratgrid::WellCurve>
sortedWells(const std::vector<paleo::stratgrid::WellCurve> &wells)
{
  std::vector<paleo::stratgrid::WellCurve> out = wells;
  std::stable_sort(out.begin(), out.end(),
                   [](const paleo::stratgrid::WellCurve &a, const paleo::stratgrid::WellCurve &b) {
                     return a.wellId < b.wellId;
                   });
  return out;
}

// 按 wellId 稳定排序后的去重曲线名（空名跳过）。provenance 与 catalog extra 共用。
QString curveProvenance(const std::vector<paleo::stratgrid::WellCurve> &wells)
{
  QStringList names;
  for (const paleo::stratgrid::WellCurve &well : sortedWells(wells))
  {
    if (well.curveName.isEmpty() || names.contains(well.curveName))
      continue;
    names.append(well.curveName);
  }
  return names.join(QLatin1Char(','));
}

struct HorizonPick
{
  QString path;
  bool found = false;
};

bool stemMatchesToken(const QString &label, const QString &token, bool *exact)
{
  const QString stem = QFileInfo(label).completeBaseName();
  if (token.isEmpty() || stem.isEmpty() || !stem.contains(token))
    return false;
  if (stem == token)
    *exact = true;
  return true;
}

HorizonPick findHorizonRaster(DataCatalog *catalog, const QString &projectDir, const QString &token)
{
  HorizonPick best;
  if (!catalog || token.isEmpty())
    return best;
  bool have = false;
  bool bestExact = false;
  int bestVersion = -1;
  QString bestAssetId;
  for (const CatalogAsset &asset : catalog->assets())
  {
    if (asset.type != QLatin1String("horizon"))
      continue;
    const QVector<CatalogVersion> versions = catalog->versionsForAsset(asset.id);
    CatalogVersion latest;
    bool haveVer = false;
    for (const CatalogVersion &version : versions)
    {
      if (!haveVer || version.versionNumber > latest.versionNumber)
      {
        latest = version;
        haveVer = true;
      }
    }
    if (!haveVer)
      continue;
    const QString path = DataCatalog::resolvedVersionPath(projectDir, latest);
    if (path.isEmpty())
      continue;
    bool matched = false;
    bool exact = false;
    if (stemMatchesToken(asset.displayName, token, &exact))
      matched = true;
    for (const CatalogVersion &version : versions)
    {
      if (stemMatchesToken(version.fileName, token, &exact))
        matched = true;
    }
    if (!matched)
      continue;
    const bool better = !have || (exact && !bestExact) ||
                        (exact == bestExact && latest.versionNumber > bestVersion) ||
                        (exact == bestExact && latest.versionNumber == bestVersion &&
                         asset.id < bestAssetId);
    if (!better)
      continue;
    have = true;
    bestExact = exact;
    bestVersion = latest.versionNumber;
    bestAssetId = asset.id;
    best.path = path;
    best.found = true;
  }
  return best;
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
  double gt[6] = {0, 1, 0, 0, 0, 1};
  if (GDALGetGeoTransform(ds, gt) != CE_None)
  {
    GDALClose(ds);
    setError(error, QStringLiteral("层位栅格没有地理参考（no georeference）：%1").arg(path));
    return false;
  }
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
  const QString trimmed = wkt.trimmed();
  if (trimmed.isEmpty())
    return out;

  const int paren = trimmed.indexOf(QLatin1Char('('));
  QString geomType = (paren < 0 ? trimmed : trimmed.left(paren)).trimmed().toUpper();
  const int semi = geomType.lastIndexOf(QLatin1Char(';'));
  if (semi >= 0)
    geomType = geomType.mid(semi + 1).trimmed();
  const QStringList parts =
      geomType.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
  geomType = parts.isEmpty() ? QString() : parts.front();
  QString dim = parts.size() >= 2 ? parts.at(1) : QString();
  if (dim.isEmpty())
  {
    if (geomType.endsWith(QLatin1String("ZM")))
    {
      dim = QStringLiteral("ZM");
      geomType.chop(2);
    }
    else if (geomType.endsWith(QLatin1String("MZ")))
    {
      dim = QStringLiteral("MZ");
      geomType.chop(2);
    }
    else if (geomType.endsWith(QLatin1Char('Z')))
    {
      dim = QStringLiteral("Z");
      geomType.chop(1);
    }
    else if (geomType.endsWith(QLatin1Char('M')))
    {
      dim = QStringLiteral("M");
      geomType.chop(1);
    }
  }
  int stride = 2;
  if (dim == QLatin1String("ZM") || dim == QLatin1String("MZ"))
    stride = 4;
  else if (dim == QLatin1String("Z") || dim == QLatin1String("M"))
    stride = 3;
  // POLYGON / MULTIPOLYGON 只取第一个至少两点的环（外环），孔洞不另作断帘。
  const bool polygon =
      geomType == QLatin1String("POLYGON") || geomType == QLatin1String("MULTIPOLYGON");

  static const QRegularExpression numRe(
      QStringLiteral("[-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?"));
  const QStringList chunks = trimmed.split(QLatin1Char('('));
  const std::size_t step = static_cast<std::size_t>(stride);
  for (const QString &chunk : chunks)
  {
    QRegularExpressionMatchIterator it = numRe.globalMatch(chunk);
    std::vector<double> nums;
    while (it.hasNext())
      nums.push_back(it.next().captured().toDouble());
    std::vector<std::pair<double, double>> pts;
    for (std::size_t i = 0; i + step <= nums.size(); i += step)
      pts.emplace_back(nums[i], nums[i + 1]);
    if (pts.size() < 2)
      continue;
    if (polygon)
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
    if (polygon)
      break;
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

  const std::vector<paleo::stratgrid::WellCurve> wells = sortedWells(request.wells);
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

PropertyModelWorkflow::PropertyModelComputed
PropertyModelWorkflow::runCompute(const PropertyModelRequest &request,
                                  const std::function<bool(double, const QString &)> &progress)
{
  PropertyModelComputed computed =
      computeSnapshot(request, m_projectDir, m_catalog && m_catalog->isOpen(), progress);
  if (!computed.ok)
    emit modelFailed(computed.error);
  return computed;
}

PropertyModelWorkflow::PropertyModelComputed
PropertyModelWorkflow::computeSnapshot(const PropertyModelRequest &request,
                                       const QString &projectDir, bool catalogOpen,
                                       const std::function<bool(double, const QString &)> &progress)
{
  PropertyModelComputed computed;
  PropertyModelOutput &result = computed.out;
  const auto fail = [&](const QString &why) {
    computed.ok = false;
    result.ok = false;
    result.error = why;
    computed.error = why;
    return computed;
  };
  const auto report = [&](double fraction, const QString &stage) {
    if (progress && !progress(fraction, stage))
    {
      result.error = QStringLiteral("已取消");
      return false;
    }
    return true;
  };

  if (!catalogOpen)
    return fail(QStringLiteral("属性建模未绑定 catalog"));
  if (request.nLayers < 1)
    return fail(QStringLiteral("层数须 ≥ 1"));
  if (!(request.idwPower > 0.0))
    return fail(QStringLiteral("IDW 幂次须为正"));

  if (!report(0.02, QStringLiteral("读层位")))
    return fail(result.error);

  paleo::stratgrid::SurfaceGrid top = request.top;
  paleo::stratgrid::SurfaceGrid bot = request.bot;
  QString topPath = request.topPath;
  QString botPath = request.botPath;
  if (!request.useEmbeddedSurfaces)
  {
    topPath = resolveSurfacePath(projectDir, request.topPath);
    botPath = resolveSurfacePath(projectDir, request.botPath);
    QString err;
    if (!loadSurface(topPath, &top, &err))
      return fail(err);
    if (!loadSurface(botPath, &bot, &err))
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
  const QString curves = curveProvenance(request.wells);
  QJsonObject prov;
  prov.insert(QStringLiteral("param_hash"), hash);
  prov.insert(QStringLiteral("property"), request.propertyName);
  prov.insert(QStringLiteral("top"), request.topName);
  prov.insert(QStringLiteral("bot"), request.botName);
  prov.insert(QStringLiteral("curve"), curves);
  prov.insert(QStringLiteral("aggregator"), paleo::stratgrid::aggregatorId(request.aggregator));
  prov.insert(QStringLiteral("n_layers"), request.nLayers);
  prov.insert(QStringLiteral("idw_power"), request.idwPower);
  prov.insert(QStringLiteral("n_wells"), static_cast<int>(request.wells.size()));
  prov.insert(QStringLiteral("n_trajectory_wells"), request.trajectoryWellCount);
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
  extra.insert(QStringLiteral("curves"), curves);

  // 登记尾巴（DerivedAssetRegistrar）不在此做——worker 线程禁止写活
  // catalog（#106 owner-thread 守卫）；由 commitComputed 在 owner 线程执行。
  computed.blob = blob;
  computed.fileName = fileName;
  computed.display = display;
  if (!request.useEmbeddedSurfaces)
    computed.parentPaths = QStringList{topPath, botPath};
  computed.extra = extra;

  result.paramHash = hash;
  result.liveColumns = volume.grid.liveColumns;
  result.filledCells = volume.filledCells;
  result.unfilledLiveCells = volume.unfilledLiveCells;
  result.volume = std::move(volume);
  computed.ok = result.ok = true;
  return computed;
}

bool PropertyModelWorkflow::commitComputed(PropertyModelComputed *computed)
{
  const auto fail = [this, computed](const QString &why) {
    computed->ok = false;
    computed->error = why;
    computed->out.ok = false;
    computed->out.error = why;
    emit modelFailed(why);
    return false;
  };

  if (!computed || !computed->ok)
    return fail(computed ? computed->error : QStringLiteral("无计算结果"));

  DerivedAssetRegistrar registrar(m_catalog, m_projectDir);
  QString stageErr;
  const DerivedStaging st = registrar.stage(QStringLiteral("property_volume"),
                                            computed->display, computed->fileName, &stageErr);
  if (!st.isValid())
    return fail(stageErr);
  QFile out(st.absolutePath);
  if (!out.open(QIODevice::WriteOnly))
    return fail(QStringLiteral("属性体写入失败：%1").arg(st.absolutePath));
  const qint64 wrote = out.write(computed->blob);
  const bool writeOk = wrote == static_cast<qint64>(computed->blob.size());
  // flush 失败先记下：close() 成功时会清掉 error()，截断文件不能 commit。
  const bool flushOk = writeOk && out.flush();
  out.close();
  if (!writeOk || !flushOk || out.error() != QFileDevice::NoError)
    return fail(QStringLiteral("属性体写入失败：%1").arg(st.absolutePath));

  const QStringList parents =
      computed->parentPaths.isEmpty() ? QStringList()
                                      : registrar.parentVersionIdsFor(computed->parentPaths);
  QString commitErr;
  if (!registrar.commit(st, parents, QStringLiteral("propertymodel/build"), computed->extra,
                        &commitErr))
    return fail(commitErr);

  computed->out.path = st.absolutePath;
  computed->out.assetId = st.assetId;
  computed->out.versionId = st.versionId;
  emit modelStored(computed->out.path);
  return true;
}

PropertyModelOutput
PropertyModelWorkflow::run(const PropertyModelRequest &request,
                           const std::function<bool(double, const QString &)> &progress)
{
  PropertyModelComputed computed = runCompute(request, progress);
  if (!computed.ok)
    return computed.out;
  if (!commitComputed(&computed))
    return computed.out;
  if (progress)
    progress(1.0, QStringLiteral("完成"));
  return computed.out;
}

PaleoTask *PropertyModelWorkflow::startJob(paleo::jobs::JobRunner<PropertyModelJob> &runner,
                                          const PropertyModelRequest &request,
                                          double overlayAlpha,
                                          std::shared_ptr<PropertyModelJob> *started)
{
  using paleo::jobs::JobRunner;

  auto job = std::make_shared<PropertyModelJob>();
  job->request = request;
  job->overlayAlpha = overlayAlpha;

  JobRunner<PropertyModelJob>::Callbacks cb;

  // prepare：owner 线程抓输入快照。request 已由调用方在 owner 线程经
  // requestFromCatalog 抓好；这里再快照工程目录与 catalog 打开态——#153：
  // worker 不再读 m_projectDir/m_catalog（工程切换时 rebind 在主线程改写
  // 它们，旧实现在 worker 上读 = 数据竞争，且会按新工程目录解析旧路径）。
  cb.prepare = [this](PropertyModelJob &j, QString *) {
    j.projectDir = m_projectDir;
    j.catalogOpen = m_catalog && m_catalog->isOpen();
    return true;
  };

  // compute：worker 线程纯计算（静态，不捕获 this）。#160：进度回调返回
  // !cancel()——任务页取消/工程切换在下一个进度点即生效，不再跑满全程。
  // #163：进度只经框架 ProgressFn → PaleoTask::reportStage（任务对象属
  // 服务，生命周期由服务排空保证）；不再捕获面板裸指针——面板随主窗口
  // 析构后 worker 仍在跑时，旧实现向已析构对象 invokeMethod。UI 侧改接
  // PaleoTask::changed 读 stage()/stagePercent()。
  cb.compute = [](PropertyModelJob &j, const paleo::jobs::CancelFn &cancel,
                  const paleo::jobs::ProgressFn &report) {
    j.computed = computeSnapshot(
        j.request, j.projectDir, j.catalogOpen,
        [&cancel, &report](double fraction, const QString &stage) {
          if (report)
            report(fraction * 100.0, stage);
          return !(cancel && cancel());
        });
    // computeSnapshot 的失败串在 computed.error 上；框架据此走失败通道。
    return j.computed.ok;
  };

  // commit：owner 线程登记。DerivedAssetRegistrar 的 stage+commit 属 #106
  // owner-thread 写守卫面，框架已断言线程亲和——这条断言正把 #80 的纪律变机制。
  cb.commit = [this](PropertyModelJob &j, QString *) {
    const bool ok = commitComputed(&j.computed);
    j.registered = ok;
    return ok;
  };

  // 取消/陈旧：不进 commit（现状「发布是临界区」）。没有临时目录要清理——
  // 属性体的 staging 由 registrar 托管，未 commit 的 staging 随请求作用域释放。
  // 取消态的 UI 文案由调用方按 reason 呈现（现状是「已取消」）。
  PaleoTask *task = runner.start(QStringLiteral("属性建模"), job, cb, QString(),
                                 /*quiet=*/true);
  if (started)
    *started = task ? job : nullptr;
  return task;
}

PropertyModelRequest PropertyModelWorkflow::requestFromCatalog(const QString &topHorizon,
                                                              const QString &bottomHorizon,
                                                              const QString &curveMnemonic,
                                                              int nLayers,
                                                              paleo::stratgrid::Aggregator aggregator,
                                                              double idwPower,
                                                              QString *error) const
{
  if (error)
    error->clear();
  if (!m_catalog || !m_catalog->isOpen())
  {
    setError(error, QStringLiteral("属性建模未绑定 catalog"));
    return {};
  }

  const HorizonPick top = findHorizonRaster(m_catalog, m_projectDir, topHorizon);
  const HorizonPick bot = findHorizonRaster(m_catalog, m_projectDir, bottomHorizon);
  if (!top.found || !bot.found)
  {
    QStringList missing;
    if (!top.found)
      missing << topHorizon;
    if (!bot.found)
      missing << bottomHorizon;
    setError(error, QStringLiteral("找不到层位栅格 %1").arg(missing.join(QStringLiteral(" / "))));
    return {};
  }

  PropertyModelRequest req;
  req.topPath = top.path;
  req.botPath = bot.path;
  req.topName = topHorizon;
  req.botName = bottomHorizon;
  req.propertyName = curveMnemonic;
  req.nLayers = nLayers;
  req.aggregator = aggregator;
  req.idwPower = idwPower;
  req.useEmbeddedSurfaces = false;

  // goal/well-trajectory：井有测斜 → 井筒站点走真实三维轨迹（x/y = 井口 +
  // 位移，z = TVD）；无测斜 → 原垂直路径（x/y 恒井口、z = MD）。直井回退
  // 是显式语义（trajectoryFor nullopt），绝不虚构造斜。
  ProjectDataFacade facade;
  facade.setCatalog(m_catalog, m_projectDir);
  int trajectoryWells = 0;

  for (const CatalogEntity &ent : m_catalog->entities(QStringLiteral("well")))
  {
    if (!ent.hasSurface)
      continue;
    const QVector<WellCurveRef> curveIndex =
        WellLogSet::wellCurveIndex(m_catalog, m_projectDir, ent.id);
    int hit = -1;
    for (int i = 0; i < curveIndex.size(); ++i)
    {
      if (curveIndex.at(i).mnemonic.compare(curveMnemonic, Qt::CaseInsensitive) == 0)
      {
        hit = i;
        break;
      }
    }
    if (hit < 0)
      continue;
    const WellCurveRef &ref = curveIndex.at(hit);
    if (ref.column < 1 || ref.path.isEmpty())
      continue;

    QStringList names;
    QList<LasCurve> curves;
    if (!LasParser::parse(ref.path, names, curves, nullptr) || ref.column >= curves.size())
      continue;

    const QVector<double> &depth = curves.at(0).values;
    const QVector<double> &vals = curves.at(ref.column).values;
    const int n = static_cast<int>(std::min(depth.size(), vals.size()));
    struct Sample
    {
      double md = 0;
      double value = 0;
    };
    std::vector<Sample> samples;
    samples.reserve(static_cast<std::size_t>(std::max(n, 0)));
    bool anyFinite = false;
    for (int i = 0; i < n; ++i)
    {
      const double md = depth.at(i);
      if (!std::isfinite(md))
        continue;
      const double value = vals.at(i);
      if (std::isfinite(value))
        anyFinite = true;
      // 保留 NaN 样点，粗化才不会把缺失段线性补上。
      samples.push_back(Sample{md, value});
    }
    if (!anyFinite)
      continue;
    std::stable_sort(samples.begin(), samples.end(),
                     [](const Sample &a, const Sample &b) { return a.md < b.md; });

    paleo::stratgrid::WellCurve well;
    well.wellId = ent.id;
    well.curveName = curveMnemonic;
    const double firstMd = samples.front().md;
    const double lastMd = samples.back().md;
    const auto survey = facade.trajectoryFor(ent.id);
    if (survey)
    {
      ++trajectoryWells;
      // 站集 = 曲线 MD 端点 + 区间内测斜站（严格递增，去重邻接同值）。
      const auto pushStationAt = [&](double md) {
        if (!well.stations.empty() && md - well.stations.back().md <= 1e-9)
          return;
        const paleo::TrajectoryPoint p = survey->pointAt(md);
        paleo::stratgrid::WellStation station;
        station.md = md;
        station.x = ent.surfaceX + p.east;
        station.y = ent.surfaceY + p.north;
        station.z = p.tvd;
        well.stations.push_back(station);
      };
      pushStationAt(firstMd);
      for (const paleo::TrajectoryPoint &sp : survey->points())
        pushStationAt(sp.md);
      pushStationAt(lastMd);
      if (lastMd > firstMd && well.stations.size() < 2)
      {
        // 端点重合护栏：曲线区间退化在单一测斜站邻域时补插值站，保住站距。
        pushStationAt(0.5 * (firstMd + lastMd) + 1e-6);
      }
    }
    else
    {
      const auto stationAt = [&](double md) {
        paleo::stratgrid::WellStation station;
        station.md = md;
        station.x = ent.surfaceX;
        station.y = ent.surfaceY;
        station.z = md;
        return station;
      };
      well.stations.push_back(stationAt(firstMd));
      if (lastMd > firstMd)
        well.stations.push_back(stationAt(lastMd));
    }
    well.curve.reserve(samples.size());
    for (const Sample &sample : samples)
      well.curve.push_back(paleo::stratgrid::CurvePoint{sample.md, sample.value});
    req.wells.push_back(std::move(well));
  }
  req.trajectoryWellCount = trajectoryWells;
  return req;
}

PropertyGridSlice PropertyModelWorkflow::gridSlice(const paleo::stratgrid::PropertyVolume &volume,
                                                   int axis, int index, QString *error)
{
  PropertyGridSlice slice;
  int width = 0;
  int height = 0;
  float vmin = 0.f;
  float vmax = 0.f;
  std::vector<float> values;
  if (!paleo::stratgrid::extractSlice(volume, axis, index, &values, &width, &height, &vmin, &vmax,
                                     error))
    return slice;
  slice.width = width;
  slice.height = height;
  slice.valueMin = vmin;
  slice.valueMax = vmax;
  slice.values = std::move(values);
  return slice;
}
