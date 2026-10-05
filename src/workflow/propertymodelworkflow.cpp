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
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
#include <QVariantMap>

#include <gdal.h>
#include <cpl_conv.h>
#include <ogr_api.h>

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
  // #127 同型：versionNumber 是资产内局部序号，跨资产不可比。跨资产的「更新」
  // 用 catalog 版本表的提交序（行序，重开稳定）。
  QHash<QString, int> commitOrder;
  {
    const QVector<CatalogVersion> all = catalog->versions();
    commitOrder.reserve(all.size());
    for (int i = 0; i < all.size(); ++i)
      commitOrder.insert(all.at(i).id, i);
  }
  int bestOrder = -1;
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
    const int order = commitOrder.value(latest.id, -1);
    const bool better = !have || (exact && !bestExact) || (exact == bestExact && order > bestOrder);
    if (!better)
      continue;
    have = true;
    bestExact = exact;
    bestOrder = order;
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

PropertyModelWorkflow::FaultThrowExtraction
PropertyModelWorkflow::throwSegmentsFromFaultSet(const paleo::fault::FaultSet &faults)
{
  FaultThrowExtraction out;
  for (const paleo::fault::Fault &fault : faults.faults())
  {
    for (const paleo::fault::FaultHorizonCut &cut : fault.cuts)
    {
      const auto segs = segmentsFromWkt(cut.wkt);
      out.curtainSegments += static_cast<int>(segs.size());
      bool throwOk = false;
      double throwZ = 0;
      const QVariant raw = cut.extra.value(QStringLiteral("throw_z"));
      if (!raw.isNull())
      {
        bool converted = false;
        throwZ = raw.toDouble(&converted);
        throwOk = converted && std::isfinite(throwZ) && throwZ != 0.0;
      }
      const bool sideKnown = cut.hangingSide == paleo::fault::FaultHangingSide::Left ||
                             cut.hangingSide == paleo::fault::FaultHangingSide::Right;
      if (throwOk && !sideKnown)
      {
        ++out.unknownSideCuts; // 有断距但盘侧未知：保持竖帘，如实计数
        continue;
      }
      for (const paleo::stratgrid::FaultSegment &seg : segs)
      {
        if (!throwOk)
          continue;
        paleo::stratgrid::FaultThrow throwSeg;
        throwSeg.x0 = seg.x0;
        throwSeg.y0 = seg.y0;
        throwSeg.x1 = seg.x1;
        throwSeg.y1 = seg.y1;
        throwSeg.throwStart = throwZ;
        throwSeg.throwEnd = throwZ;
        throwSeg.dropLeftSide = cut.hangingSide == paleo::fault::FaultHangingSide::Left;
        out.throws.push_back(throwSeg);
        ++out.throwSegments;
      }
    }
  }
  return out;
}

QString PropertyModelWorkflow::paramHash(const PropertyModelRequest &request,
                                         const paleo::stratgrid::SurfaceGrid &top,
                                         const paleo::stratgrid::SurfaceGrid &bot)
{
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData("paleo-propmodel-v2\n");
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
  // V2 输入面：方法 / 断距 / SGS / 对象 / 相带（同输入同哈希，无时间戳）。
  hash.addData(request.method == PropertyMethod::Sgs ? "sgs\n" : "idw\n");
  for (const paleo::stratgrid::FaultThrow &t : request.faultThrows)
  {
    hash.addData(num(t.x0).toUtf8());
    hash.addData(",");
    hash.addData(num(t.y0).toUtf8());
    hash.addData(",");
    hash.addData(num(t.x1).toUtf8());
    hash.addData(",");
    hash.addData(num(t.y1).toUtf8());
    hash.addData(",");
    hash.addData(num(t.throwStart).toUtf8());
    hash.addData(",");
    hash.addData(num(t.throwEnd).toUtf8());
    hash.addData(",");
    hash.addData(t.dropLeftSide ? "L" : "R");
    hash.addData(";");
  }
  if (request.method == PropertyMethod::Sgs)
  {
    hash.addData(QByteArray::number(request.sgsRealizations));
    hash.addData(",");
    hash.addData(QByteArray::number(request.sgsSeed));
    hash.addData(",");
    hash.addData(QByteArray::number(request.sgsMaxPoints));
    hash.addData(",");
    hash.addData(QByteArray::number(static_cast<int>(request.variogram.type)));
    hash.addData(",");
    hash.addData(num(request.variogram.nugget).toUtf8());
    hash.addData(",");
    hash.addData(num(request.variogram.sill).toUtf8());
    hash.addData(",");
    hash.addData(num(request.variogram.range).toUtf8());
    hash.addData(",");
    hash.addData(num(request.variogram.anisotropyRatio).toUtf8());
    hash.addData(",");
    hash.addData(num(request.variogram.azimuthDeg).toUtf8());
    hash.addData(",");
    hash.addData(num(request.variogram.verticalRangeRatio).toUtf8());
    hash.addData("\n");
  }
  for (const paleo::stratgrid::ObjectSpec &spec : request.objectSpecs)
  {
    hash.addData(QByteArray::number(request.objectSeed));
    hash.addData("|");
    hash.addData(QByteArray::number(static_cast<int>(spec.type)));
    hash.addData(",");
    hash.addData(num(spec.azimuthDeg).toUtf8());
    hash.addData(",");
    hash.addData(num(spec.length).toUtf8());
    hash.addData(",");
    hash.addData(num(spec.width).toUtf8());
    hash.addData(",");
    hash.addData(num(spec.thickness).toUtf8());
    hash.addData(",");
    hash.addData(num(spec.curvature).toUtf8());
    hash.addData(",");
    hash.addData(num(spec.verticalFrac).toUtf8());
    hash.addData(",");
    hash.addData(num(spec.value).toUtf8());
    hash.addData(",");
    hash.addData(QByteArray::number(spec.count));
    hash.addData(",");
    hash.addData(QByteArray::number(spec.zoneCode));
    hash.addData(";");
  }
  if (request.useFacies)
  {
    for (const paleo::stratgrid::ZoneRing &ring : request.faciesRings)
    {
      hash.addData(QByteArray::number(ring.code));
      hash.addData(":");
      for (std::size_t i = 0; i < ring.xs.size() && i < ring.ys.size(); ++i)
      {
        hash.addData(num(ring.xs[i]).toUtf8());
        hash.addData(",");
        hash.addData(num(ring.ys[i]).toUtf8());
        hash.addData(";");
      }
    }
  }
  return QString::fromLatin1(hash.result().toHex());
}

PropertyModelWorkflow::PropertyModelComputedList
PropertyModelWorkflow::runCompute(const PropertyModelRequest &request,
                                  const std::function<bool(double, const QString &)> &progress)
{
  PropertyModelComputedList computed =
      computeSnapshot(request, m_projectDir, m_catalog && m_catalog->isOpen(), progress);
  if (computed.empty() || !computed.front().ok)
    emit modelFailed(computed.empty() ? QStringLiteral("属性建模失败") : computed.front().error);
  return computed;
}

PropertyModelWorkflow::PropertyModelComputedList
PropertyModelWorkflow::computeSnapshot(const PropertyModelRequest &request,
                                       const QString &projectDir, bool catalogOpen,
                                       const std::function<bool(double, const QString &)> &progress)
{
  PropertyModelComputedList results;
  const auto fail = [&](const QString &why) {
    PropertyModelComputed bad;
    bad.ok = false;
    bad.error = why;
    bad.out.ok = false;
    bad.out.error = why;
    results.clear();
    results.push_back(bad);
    return results;
  };
  const auto report = [&](double fraction, const QString &stage) {
    if (progress && !progress(fraction, stage))
      return false;
    return true;
  };

  if (!catalogOpen)
    return fail(QStringLiteral("属性建模未绑定 catalog"));
  if (request.nLayers < 1)
    return fail(QStringLiteral("层数须 ≥ 1"));
  if (request.method == PropertyMethod::Idw && !(request.idwPower > 0.0))
    return fail(QStringLiteral("IDW 幂次须为正"));
  if (request.method == PropertyMethod::Sgs)
  {
    const auto &model = request.variogram;
    if (!(model.range > 0.0) || model.nugget < 0.0 || model.sill < 0.0 ||
        !(model.nugget + model.sill > 0.0))
      return fail(QStringLiteral("变差模型非法：变程须为正，块金/基台非负且总基台为正"));
    if (request.sgsRealizations < 1 || request.sgsRealizations > 64)
      return fail(QStringLiteral("实现数须在 [1, 64]"));
  }

  if (!report(0.02, QStringLiteral("读层位")))
    return fail(QStringLiteral("已取消"));

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
    return fail(QStringLiteral("已取消"));

  paleo::stratgrid::ZoneGrid grid;
  QString err;
  if (!paleo::stratgrid::buildZoneGrid(top, bot, request.nLayers, &grid, &err))
    return fail(err);

  // 断块错位：断距矢量驱动下掉侧柱平移（无断距 = 纯竖帘，V1 行为不变）。
  paleo::stratgrid::FaultOffsetMeta offsetMeta;
  QString offsetCaliber;
  if (!request.faultThrows.empty())
  {
    if (!report(0.13, QStringLiteral("断块错位")))
      return fail(QStringLiteral("已取消"));
    std::vector<float> dz;
    if (!paleo::stratgrid::applyFaultOffset(grid, request.faultThrows, &grid, &dz, &offsetMeta,
                                            &err))
      return fail(err);
    offsetCaliber = QStringLiteral("断块错位：%1 段断距（最大 |throw| = %2 m）、错动 %3 柱")
                        .arg(request.faultThrows.size())
                        .arg(offsetMeta.maxAbsThrow, 0, 'g', 6)
                        .arg(offsetMeta.offsetColumns);
  }
  else
  {
    offsetCaliber = QStringLiteral("无断距输入：断层保持竖帘（无几何错位）");
  }

  if (!report(0.20, QStringLiteral("粗化")))
    return fail(QStringLiteral("已取消"));

  paleo::stratgrid::UpscaleTable table;
  if (!paleo::stratgrid::upscaleWells(grid, request.wells, request.aggregator, &table, &err))
    return fail(err);
  const std::vector<paleo::stratgrid::Seed> seeds = paleo::stratgrid::seedsFromUpscale(table);

  // 相带栅格化（owner 线程已收集好多边形环；worker 只做纯几何）。
  std::vector<int> zones;
  const std::vector<int> *zonePtr = nullptr;
  QString faciesCaliber;
  if (request.useFacies && !request.faciesRings.empty())
  {
    if (!report(0.24, QStringLiteral("相带栅格")))
      return fail(QStringLiteral("已取消"));
    zones = paleo::stratgrid::rasterizeZoneRings(grid, request.faciesRings);
    zonePtr = &zones;
    int matched = 0;
    for (int code : zones)
      if (code >= 0)
        ++matched;
    faciesCaliber = QStringLiteral("相带面 %1：%2 环、命中柱 %3、背景域柱 %4（无覆盖区自成参数域）")
                        .arg(request.faciesAssetName.isEmpty() ? QStringLiteral("(未命名)")
                                                               : request.faciesAssetName)
                        .arg(request.faciesRings.size())
                        .arg(matched)
                        .arg(static_cast<int>(zones.size()) - matched);
  }
  else
  {
    faciesCaliber = QStringLiteral("无相带输入：全域单一参数域");
  }

  // 背景场：IDW（单实现）或 SGS（多实现）。
  std::vector<paleo::stratgrid::PropertyVolume> volumes;
  QString fillCaliber;
  int sgsMergedDuplicates = 0;
  int sgsSnappedCells = 0;
  int sgsSolverFailures = 0;
  if (request.method == PropertyMethod::Sgs)
  {
    paleo::geostat::Sgs3Params params;
    params.nRealizations = request.sgsRealizations;
    params.seed = request.sgsSeed;
    params.maxPoints = request.sgsMaxPoints;
    paleo::stratgrid::SgsFillMeta meta;
    if (!paleo::stratgrid::fillSgs(grid, seeds, request.faults, zonePtr, request.variogram,
                                   params, &volumes, &meta,
                                   [&](double fraction) {
                                     return report(0.30 + 0.60 * fraction, QStringLiteral("序贯高斯"));
                                   },
                                   &err))
      return fail(err.isEmpty() ? QStringLiteral("已取消") : err);
    fillCaliber = meta.caliber;
    sgsMergedDuplicates = meta.mergedSeedDuplicates;
    sgsSnappedCells = meta.snappedSeedCells;
    sgsSolverFailures = meta.solverFailures;
  }
  else
  {
    paleo::stratgrid::PropertyVolume volume;
    const bool filled = paleo::stratgrid::fillIdw(
        grid, seeds, request.faults, request.idwPower, &volume,
        [&](double fraction) { return report(0.30 + 0.60 * fraction, QStringLiteral("充填")); }, &err);
    if (!filled)
      return fail(err.isEmpty() ? QStringLiteral("已取消") : err);
    volumes.push_back(std::move(volume));
    fillCaliber = QStringLiteral("IDW：幂次 %1，分块竖帘阻断").arg(request.idwPower);
  }

  // 对象建模：硬覆盖背景场（口径钉死：对象优先）。
  paleo::stratgrid::ObjectModelMeta objectMeta;
  if (!request.objectSpecs.empty())
  {
    if (!report(0.92, QStringLiteral("对象建模")))
      return fail(QStringLiteral("已取消"));
    paleo::stratgrid::PropertyVolume objects;
    if (!paleo::stratgrid::placeObjects(grid, zonePtr, request.objectSeed, request.objectSpecs,
                                        &objects, &objectMeta, &err))
      return fail(err);
    for (paleo::stratgrid::PropertyVolume &volume : volumes)
    {
      if (!paleo::stratgrid::applyObjectOverride(objects, &volume, &err))
        return fail(err);
    }
  }

  if (!report(0.94, QStringLiteral("落盘")))
    return fail(QStringLiteral("已取消"));

  const QString hash = paramHash(request, top, bot);
  const QString curves = curveProvenance(request.wells);
  const int verticalWells =
      request.trajectoryWellCount >= 0
          ? std::max(0, static_cast<int>(request.wells.size()) - request.trajectoryWellCount)
          : 0;
  const QString trajectoryCaliber =
      QStringLiteral("%1/%2 井真实轨迹（余为竖直近似）")
          .arg(request.trajectoryWellCount)
          .arg(static_cast<int>(request.wells.size()));

  QString fileStem = request.propertyName.trimmed();
  if (fileStem.isEmpty())
    fileStem = QStringLiteral("PROP");
  fileStem.replace(QLatin1Char('/'), QLatin1Char('_'));
  fileStem.replace(QLatin1Char('\\'), QLatin1Char('_'));
  const bool multi = volumes.size() > 1;
  const QString display = QStringLiteral("%1 %2-%3")
                              .arg(request.propertyName.isEmpty() ? QStringLiteral("PROP")
                                                                  : request.propertyName,
                                   request.topName, request.botName);

  results.reserve(volumes.size());
  for (std::size_t r = 0; r < volumes.size(); ++r)
  {
    const paleo::stratgrid::PropertyVolume &volume = volumes[r];
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
    prov.insert(QStringLiteral("n_vertical_approx_wells"), verticalWells);
    prov.insert(QStringLiteral("trajectory_caliber"), trajectoryCaliber);
    prov.insert(QStringLiteral("n_fault_segments"), static_cast<int>(request.faults.size()));
    prov.insert(QStringLiteral("method"),
                request.method == PropertyMethod::Sgs ? QStringLiteral("sgs")
                                                      : QStringLiteral("idw"));
    prov.insert(QStringLiteral("caliber"),
                QStringList{offsetCaliber, faciesCaliber, fillCaliber,
                            objectMeta.placements.empty()
                                ? QStringLiteral("无对象建模")
                                : objectMeta.caliber,
                            trajectoryCaliber}
                    .join(QStringLiteral(" ｜ ")));
    if (request.method == PropertyMethod::Sgs)
    {
      prov.insert(QStringLiteral("realization_index"), static_cast<int>(r));
      prov.insert(QStringLiteral("realization_count"), static_cast<int>(volumes.size()));
      prov.insert(QStringLiteral("seed"), QString::number(request.sgsSeed));
      prov.insert(QStringLiteral("variogram_type"), static_cast<int>(request.variogram.type));
      prov.insert(QStringLiteral("nugget"), request.variogram.nugget);
      prov.insert(QStringLiteral("sill"), request.variogram.sill);
      prov.insert(QStringLiteral("range"), request.variogram.range);
      prov.insert(QStringLiteral("anisotropy_ratio"), request.variogram.anisotropyRatio);
      prov.insert(QStringLiteral("azimuth_deg"), request.variogram.azimuthDeg);
      prov.insert(QStringLiteral("vertical_range_ratio"), request.variogram.verticalRangeRatio);
      prov.insert(QStringLiteral("sgs_merged_duplicates"), sgsMergedDuplicates);
      prov.insert(QStringLiteral("sgs_snapped_seed_cells"), sgsSnappedCells);
      prov.insert(QStringLiteral("sgs_solver_failures"), sgsSolverFailures);
    }
    if (!request.faultThrows.empty())
    {
      prov.insert(QStringLiteral("fault_offset_columns"), offsetMeta.offsetColumns);
      prov.insert(QStringLiteral("fault_offset_boundary_columns"), offsetMeta.boundaryColumns);
      prov.insert(QStringLiteral("fault_offset_max_abs_throw"), offsetMeta.maxAbsThrow);
    }
    if (!objectMeta.placements.empty())
    {
      QJsonArray placements;
      for (const paleo::stratgrid::ObjectPlacementRecord &record : objectMeta.placements)
      {
        QJsonObject entry;
        entry.insert(QStringLiteral("type"), record.type == paleo::stratgrid::ObjectType::Channel
                                                  ? QStringLiteral("channel")
                                                  : QStringLiteral("point_bar"));
        entry.insert(QStringLiteral("center_x"), record.centerX);
        entry.insert(QStringLiteral("center_y"), record.centerY);
        entry.insert(QStringLiteral("azimuth_deg"), record.azimuthDeg);
        entry.insert(QStringLiteral("length"), record.length);
        entry.insert(QStringLiteral("width"), record.width);
        entry.insert(QStringLiteral("thickness"), record.thickness);
        entry.insert(QStringLiteral("curvature"), record.curvature);
        entry.insert(QStringLiteral("vertical_frac"), record.verticalFrac);
        entry.insert(QStringLiteral("value"), record.value);
        entry.insert(QStringLiteral("cells"), record.cells);
        entry.insert(QStringLiteral("zone_code"), record.zoneCode);
        placements.append(entry);
      }
      prov.insert(QStringLiteral("object_placements"), placements);
      prov.insert(QStringLiteral("object_cells"), objectMeta.objectCells);
      prov.insert(QStringLiteral("object_seed"), QString::number(request.objectSeed));
    }
    const QByteArray blob = paleo::stratgrid::writePropertyBlob(volume, prov);
    if (blob.isEmpty())
      return fail(QStringLiteral("属性体序列化失败"));

    PropertyModelComputed computed;
    computed.blob = blob;
    computed.fileName =
        multi ? QStringLiteral("PROP_%1_R%2.pprop").arg(fileStem).arg(r + 1, 4, 10, QLatin1Char('0'))
              : QStringLiteral("PROP_%1.pprop").arg(fileStem);
    computed.display = display;
    if (!request.useEmbeddedSurfaces)
      computed.parentPaths = QStringList{topPath, botPath};

    QVariantMap extra;
    extra.insert(QStringLiteral("param_hash"), hash);
    extra.insert(QStringLiteral("property"), request.propertyName);
    extra.insert(QStringLiteral("top_name"), request.topName);
    extra.insert(QStringLiteral("bot_name"), request.botName);
    extra.insert(QStringLiteral("aggregator"), paleo::stratgrid::aggregatorId(request.aggregator));
    extra.insert(QStringLiteral("n_layers"), request.nLayers);
    extra.insert(QStringLiteral("idw_power"), request.idwPower);
    extra.insert(QStringLiteral("method"),
                 request.method == PropertyMethod::Sgs ? QStringLiteral("sgs")
                                                       : QStringLiteral("idw"));
    extra.insert(QStringLiteral("ni"), volume.grid.ni);
    extra.insert(QStringLiteral("nj"), volume.grid.nj);
    extra.insert(QStringLiteral("nk"), volume.grid.nk);
    extra.insert(QStringLiteral("live_columns"), volume.grid.liveColumns);
    extra.insert(QStringLiteral("filled_cells"), volume.filledCells);
    extra.insert(QStringLiteral("unfilled_live_cells"), volume.unfilledLiveCells);
    extra.insert(QStringLiteral("curves"), curves);
    extra.insert(QStringLiteral("n_trajectory_wells"), request.trajectoryWellCount);
    extra.insert(QStringLiteral("n_vertical_approx_wells"), verticalWells);
    extra.insert(QStringLiteral("caliber"), prov.value(QStringLiteral("caliber")).toString());
    if (request.method == PropertyMethod::Sgs)
    {
      extra.insert(QStringLiteral("realization_index"), static_cast<int>(r));
      extra.insert(QStringLiteral("realization_count"), static_cast<int>(volumes.size()));
      extra.insert(QStringLiteral("seed"), QString::number(request.sgsSeed));
    }
    if (!request.faultThrows.empty())
      extra.insert(QStringLiteral("fault_offset_columns"), offsetMeta.offsetColumns);
    if (!objectMeta.placements.empty())
      extra.insert(QStringLiteral("object_cells"), objectMeta.objectCells);
    computed.extra = extra;

    PropertyModelOutput &result = computed.out;
    result.paramHash = hash;
    result.realizationCount = static_cast<int>(volumes.size());
    result.liveColumns = volume.grid.liveColumns;
    result.filledCells = volume.filledCells;
    result.unfilledLiveCells = volume.unfilledLiveCells;
    result.volume = volume;
    computed.ok = result.ok = true;
    results.push_back(std::move(computed));
  }
  return results;
}

bool PropertyModelWorkflow::commitAll(PropertyModelComputedList *list)
{
  if (!list || list->empty())
  {
    emit modelFailed(QStringLiteral("无计算结果"));
    return false;
  }
  for (PropertyModelComputed &computed : *list)
  {
    if (!commitComputed(&computed))
      return false;
  }
  return true;
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
  PropertyModelComputedList computed = runCompute(request, progress);
  if (computed.empty() || !computed.front().ok)
    return computed.empty() ? PropertyModelOutput{} : computed.front().out;
  if (!commitAll(&computed))
    return computed.front().out;
  if (progress)
    progress(1.0, QStringLiteral("完成"));
  return computed.front().out;
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
    // computeSnapshot 的失败串在首元素 error 上；框架据此走失败通道。
    return !j.computed.empty() && j.computed.front().ok;
  };

  // commit：owner 线程登记。DerivedAssetRegistrar 的 stage+commit 属 #106
  // owner-thread 写守卫面，框架已断言线程亲和——这条断言正把 #80 的纪律变机制。
  cb.commit = [this](PropertyModelJob &j, QString *) {
    const bool ok = commitAll(&j.computed);
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
    // #166：深度道单位 → 米制 MD（轨迹/层位都是米）。FT 族 ×0.3048；
    // 空/未知单位沿用旧口径按米处理。
    const double unitScale = LasParser::depthUnitToMeters(curves.at(0).unit);
    const double mdScale = unitScale > 0.0 ? unitScale : 1.0;
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
      const double md = depth.at(i) * mdScale;
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

namespace
{

// WKT POLYGON/MULTIPOLYGON → 外环坐标对（孔洞不另取——编图产物是无重叠
// 邻接瓦片，孔洞口径递延）。解析不出 ≥3 点 → 空。
std::vector<std::vector<std::pair<double, double>>>
outerRingsFromWkt(const QString &wkt)
{
  std::vector<std::vector<std::pair<double, double>>> rings;
  const QString trimmed = wkt.trimmed();
  if (!trimmed.contains(QLatin1String("POLYGON"), Qt::CaseInsensitive))
    return rings;
  static const QRegularExpression numRe(
      QStringLiteral("[-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?"));
  const QStringList chunks = trimmed.split(QLatin1Char('('));
  for (const QString &chunk : chunks)
  {
    QRegularExpressionMatchIterator it = numRe.globalMatch(chunk);
    std::vector<double> nums;
    while (it.hasNext())
      nums.push_back(it.next().captured().toDouble());
    std::vector<std::pair<double, double>> pts;
    for (std::size_t i = 0; i + 1 < nums.size(); i += 2)
      pts.emplace_back(nums[i], nums[i + 1]);
    if (pts.size() >= 3)
      rings.push_back(std::move(pts));
  }
  return rings;
}

} // namespace

bool PropertyModelWorkflow::collectFaciesPolygons(PropertyModelRequest *request,
                                                  QString *error) const
{
  if (!request)
  {
    setError(error, QStringLiteral("请求为空"));
    return false;
  }
  request->faciesRings.clear();
  request->faciesAssetName.clear();
  if (!m_catalog || !m_catalog->isOpen())
  {
    setError(error, QStringLiteral("属性建模未绑定 catalog"));
    return false;
  }

  // 最新 facies_draft_map 版本（#127 口径：跨资产比较用版本表提交序）。
  QString bestPath;
  QString bestName;
  int bestOrder = -1;
  QHash<QString, int> commitOrder;
  {
    const QVector<CatalogVersion> all = m_catalog->versions();
    commitOrder.reserve(all.size());
    for (int i = 0; i < all.size(); ++i)
      commitOrder.insert(all.at(i).id, i);
  }
  for (const CatalogAsset &asset : m_catalog->assets())
  {
    if (asset.type != QLatin1String("facies_draft_map"))
      continue;
    const QVector<CatalogVersion> versions = m_catalog->versionsForAsset(asset.id);
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
    const QString path = DataCatalog::resolvedVersionPath(m_projectDir, latest);
    if (path.isEmpty())
      continue;
    const int order = commitOrder.value(latest.id, -1);
    if (order > bestOrder)
    {
      bestOrder = order;
      bestPath = path;
      bestName = asset.displayName;
    }
  }
  if (bestPath.isEmpty())
  {
    setError(error, QStringLiteral("没有可用的相带草稿图资产（facies_draft_map）"));
    return false;
  }

  // OGR 读 GPKG：facies_code 字段 + 多边形外环。CRS 不做重投影——相带图
  // 出自同工程编图链（同工程 CRS），跨 CRS 消费递延（ledger 记档）。
  GDALAllRegister();
  GDALDatasetH ds = OGROpen(bestPath.toUtf8().constData(), FALSE, nullptr);
  if (!ds)
  {
    setError(error, QStringLiteral("相带图打不开：%1").arg(bestPath));
    return false;
  }
  const int nLayers = GDALDatasetGetLayerCount(ds);
  OGRLayerH layer = nLayers > 0 ? GDALDatasetGetLayer(ds, 0) : nullptr;
  if (!layer)
  {
    GDALClose(ds);
    setError(error, QStringLiteral("相带图没有图层：%1").arg(bestPath));
    return false;
  }
  OGR_L_ResetReading(layer);
  int badCode = 0;
  int badGeom = 0;
  while (true)
  {
    OGRFeatureH feature = OGR_L_GetNextFeature(layer);
    if (!feature)
      break;
    // OGR C API 的字段索引按 feature 查（层级 GetFieldIndex 不在 C 面）。
    const int codeField = OGR_F_GetFieldIndex(feature, "facies_code");
    int code = -1;
    bool codeOk = false;
    if (codeField >= 0 && OGR_F_IsFieldSetAndNotNull(feature, codeField))
    {
      code = OGR_F_GetFieldAsInteger(feature, codeField);
      codeOk = true;
    }
    OGRGeometryH geometry = OGR_F_GetGeometryRef(feature);
    char *wkt = nullptr;
    if (!codeOk || code < 0 || !geometry ||
        OGR_G_ExportToWkt(geometry, &wkt) != OGRERR_NONE || !wkt)
    {
      if (!codeOk || code < 0)
        ++badCode;
      else
        ++badGeom;
      OGR_F_Destroy(feature);
      continue;
    }
    const auto rings = outerRingsFromWkt(QString::fromLatin1(wkt));
    CPLFree(wkt);
    OGR_F_Destroy(feature);
    if (rings.empty())
    {
      ++badGeom;
      continue;
    }
    for (const auto &pts : rings)
    {
      paleo::stratgrid::ZoneRing ring;
      ring.code = code;
      ring.xs.reserve(pts.size());
      ring.ys.reserve(pts.size());
      for (const auto &pt : pts)
      {
        ring.xs.push_back(pt.first);
        ring.ys.push_back(pt.second);
      }
      request->faciesRings.push_back(std::move(ring));
    }
  }
  GDALClose(ds);
  request->faciesAssetName = bestName;
  request->useFacies = true;
  if (request->faciesRings.empty())
  {
    setError(error,
             QStringLiteral("相带图 %1 没有可用多边形（facies_code<0 %2 个、几何失败 %3 个）")
                 .arg(bestName)
                 .arg(badCode)
                 .arg(badGeom));
    return false;
  }
  return true;
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
