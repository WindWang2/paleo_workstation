// 层：数据
#include "io/attrgridout.h"

#include <QDir>
#include <QFileInfo>

#include <gdal.h>
#include <cpl_string.h>
#include <ogr_spatialref.h>

#include <cmath>
#include <cstring>

#include "catalog/datacatalog.h"

namespace paleo::sattr
{

namespace
{

constexpr float kNoData = -9999.f;

void setError(QString *error, const QString &text)
{
  if (error)
    *error = text;
}

// 轴均值步长（稀疏/非均匀轴的 geotransform 是均值近似——元数据里另记
// PALEO_AXIS_REGULAR 让消费方知情，不冒充精确）。
double meanStep(const std::vector<int> &axis)
{
  if (axis.size() < 2)
    return 0.0;
  return double(axis.back() - axis.front()) / double(axis.size() - 1);
}

bool axisRegular(const std::vector<int> &axis)
{
  if (axis.size() < 2)
    return true;
  const int step = axis[1] - axis[0];
  for (std::size_t i = 2; i < axis.size(); ++i)
    if (axis[i] - axis[i - 1] != step)
      return false;
  return true;
}

} // namespace

bool writeTimeSliceGeoTiff(const QString &path, const AttrTimeSliceGrid &grid,
                           const double affine[6], QString *error)
{
  if (grid.nIl <= 0 || grid.nXl <= 0 ||
      grid.ilValues.size() != std::size_t(grid.nIl) ||
      grid.xlValues.size() != std::size_t(grid.nXl) ||
      grid.values.size() != std::size_t(grid.nIl) * grid.nXl)
  {
    setError(error, QStringLiteral("时间切片网格几何/轴值表/值块不符"));
    return false;
  }
  const double dRow = meanStep(grid.ilValues);
  const double dCol = meanStep(grid.xlValues);
  if (!(dRow > 0.0) || !(dCol > 0.0))
  {
    setError(error, QStringLiteral("测网轴步长非正（单线轴不可地理栅格化）"));
    return false;
  }
  if (!affine)
  {
    setError(error, QStringLiteral("地理参考仿射缺失"));
    return false;
  }
  const double a = affine[0], b = affine[1], c = affine[2];
  const double d = affine[3], e = affine[4], f = affine[5];

  const QDir dir = QFileInfo(path).absoluteDir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
  {
    setError(error, QStringLiteral("无法创建目录 %1").arg(dir.absolutePath()));
    return false;
  }
  GDALAllRegister();
  GDALDriverH drv = GDALGetDriverByName("GTiff");
  if (!drv)
  {
    setError(error, QStringLiteral("GTiff driver 不可用"));
    return false;
  }
  GDALDatasetH ds = GDALCreate(drv, path.toUtf8().constData(), grid.nXl,
                               grid.nIl, 1, GDT_Float32, nullptr);
  if (!ds)
  {
    setError(error, QStringLiteral("无法创建 %1：%2")
                             .arg(path, QString::fromUtf8(CPLGetLastErrorMsg())));
    return false;
  }

  // 全参 geotransform：像元角点对应轴首结点外半像元；行=inline 升序、
  // 列=crossline 升序；旋转项（GT2/GT4）来自测网仿射，如实保留。
  const double gtCol[2] = {b * dCol, e * dCol}; // 每列步进 (x, y)
  const double gtRow[2] = {a * dRow, d * dRow}; // 每行步进 (x, y)
  const double x0 = a * grid.ilValues.front() + b * grid.xlValues.front() + c;
  const double y0 = d * grid.ilValues.front() + e * grid.xlValues.front() + f;
  double gt[6];
  gt[0] = x0 - 0.5 * (gtCol[0] + gtRow[0]);
  gt[1] = gtCol[0];
  gt[2] = gtRow[0];
  gt[3] = y0 - 0.5 * (gtCol[1] + gtRow[1]);
  gt[4] = gtCol[1];
  gt[5] = gtRow[1];
  GDALSetGeoTransform(ds, gt);
  GDALSetRasterNoDataValue(GDALGetRasterBand(ds, 1), kNoData);

  const auto setMetaStr = [ds](const char *key, const QString &value) {
    const QByteArray encoded = value.toUtf8();
    return GDALSetMetadataItem(ds, key, encoded.constData(), nullptr) == CE_None;
  };
  const auto setMetaInt = [ds](const char *key, qint64 value) {
    const QByteArray encoded = QByteArray::number(value);
    return GDALSetMetadataItem(ds, key, encoded.constData(), nullptr) == CE_None;
  };
  const auto setMetaDbl = [ds](const char *key, double value) {
    // 'g' 15：导航/复验消费方读到全精度值（默认 6 位会把 TWT/值域截尾）。
    const QByteArray encoded = QByteArray::number(value, 'g', 15);
    return GDALSetMetadataItem(ds, key, encoded.constData(), nullptr) == CE_None;
  };
  const bool axisRegularFlags =
      axisRegular(grid.ilValues) && axisRegular(grid.xlValues);
  if (!setMetaStr("PALEO_ATTR_ID", grid.attrId) ||
      !setMetaInt("PALEO_TIME_SAMPLE_INDEX", grid.sampleIndex) ||
      !setMetaDbl("PALEO_TIME_MS", grid.timeMs) ||
      !setMetaDbl("PALEO_DT_MS", grid.sampleIntervalMs) ||
      !setMetaDbl("PALEO_T0_MS", grid.startTimeMs) ||
      !setMetaInt("PALEO_INLINE_MIN", grid.ilValues.front()) ||
      !setMetaInt("PALEO_INLINE_MAX", grid.ilValues.back()) ||
      !setMetaInt("PALEO_XLINE_MIN", grid.xlValues.front()) ||
      !setMetaInt("PALEO_XLINE_MAX", grid.xlValues.back()) ||
      !setMetaInt("PALEO_AXIS_REGULAR", axisRegularFlags ? 1 : 0) ||
      !setMetaInt("PALEO_VALID_CELLS", grid.validCells) ||
      !setMetaDbl("PALEO_VALUE_MIN", grid.valueMin) ||
      !setMetaDbl("PALEO_VALUE_MAX", grid.valueMax) ||
      (grid.paramHash.isEmpty() ||
       !setMetaStr("PALEO_PARAM_HASH", grid.paramHash)) ||
      (grid.sourceSgyPath.isEmpty() ||
       !setMetaStr("PALEO_SOURCE_SGY", grid.sourceSgyPath)))
  {
    const QString detail = QString::fromUtf8(CPLGetLastErrorMsg());
    GDALClose(ds);
    setError(error, QStringLiteral("栅格元数据写入失败：%1%2")
                         .arg(path, detail.isEmpty() ? QString()
                                                     : QStringLiteral(": ") + detail));
    return false;
  }

  // 局部直角米测网 CRS（horizonbinner 同约定）：无大地基准的 ENGCRS。
  // 层树条目承诺「一定地理参考」——CRS 构造失败视为写失败（宁可不出
  // 栅格，不出会让 QGIS 弹 CRS 询问的无投影假栅格）。
  OGRSpatialReference srs;
  char *wkt = nullptr;
  if (srs.SetFromUserInput(DataCatalog::localGridCrsWkt().toUtf8().constData()) !=
          OGRERR_NONE ||
      srs.exportToWkt(&wkt) != OGRERR_NONE || !wkt)
  {
    CPLFree(wkt);
    GDALClose(ds);
    QFile::remove(path);
    setError(error, QStringLiteral("局部测网 CRS 构造失败：%1").arg(path));
    return false;
  }
  GDALSetProjection(ds, wkt);
  CPLFree(wkt);

  // 逐行写（NaN → nodata；单行缓冲，大测网不整幅驻留）。
  std::vector<float> row(std::size_t(grid.nXl));
  for (int r = 0; r < grid.nIl; ++r)
  {
    std::memcpy(row.data(), &grid.values[std::size_t(r) * grid.nXl],
                std::size_t(grid.nXl) * sizeof(float));
    for (int x = 0; x < grid.nXl; ++x)
      if (std::isnan(row[std::size_t(x)]))
        row[std::size_t(x)] = kNoData;
    if (GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, r, grid.nXl, 1,
                     row.data(), grid.nXl, 1, GDT_Float32, 0, 0) != CE_None)
    {
      GDALClose(ds);
      QFile::remove(path); // 失败不留半成品栅格
      setError(error, QStringLiteral("栅格像元写入失败（行 %1）：%2")
                           .arg(r).arg(path));
      return false;
    }
  }
  GDALClose(ds);
  return true;
}

bool readTimeSliceGeoTiffSummary(const QString &path,
                                 AttrTimeSliceGrid *summary, QString *error)
{
  if (!summary)
  {
    setError(error, QStringLiteral("摘要输出为空"));
    return false;
  }
  GDALAllRegister();
  GDALDatasetH ds = GDALOpen(path.toUtf8().constData(), GA_ReadOnly);
  if (!ds)
  {
    setError(error, QStringLiteral("缓存栅格无法打开：%1").arg(path));
    return false;
  }
  const CSLConstList meta = GDALGetMetadata(ds, nullptr);
  const auto metaStr = [&meta](const char *key) -> QString {
    const char *v = CSLFetchNameValue(meta, key);
    return v ? QString::fromUtf8(v) : QString();
  };
  bool ok = true;
  const QString attrId = metaStr("PALEO_ATTR_ID");
  const QString hash = metaStr("PALEO_PARAM_HASH");
  const QString validStr = metaStr("PALEO_VALID_CELLS");
  const QString vmin = metaStr("PALEO_VALUE_MIN");
  const QString vmax = metaStr("PALEO_VALUE_MAX");
  const QString tms = metaStr("PALEO_TIME_MS");
  const QString dt = metaStr("PALEO_DT_MS");
  const QString t0 = metaStr("PALEO_T0_MS");
  const QString sidx = metaStr("PALEO_TIME_SAMPLE_INDEX");
  if (attrId.isEmpty() || hash.isEmpty() || validStr.isEmpty() ||
      vmin.isEmpty() || vmax.isEmpty() || sidx.isEmpty())
  {
    ok = false;
    setError(error, QStringLiteral("缓存栅格缺 PALEO_* 身份元数据：%1")
                             .arg(path));
  }
  double gt[6] = {0, 0, 0, 0, 0, 0};
  if (ok && GDALGetGeoTransform(ds, gt) != CE_None)
  {
    ok = false;
    setError(error, QStringLiteral("缓存栅格地理参考缺失：%1").arg(path));
  }
  if (ok)
  {
    summary->attrId = attrId;
    summary->paramHash = hash;
    summary->nIl = GDALGetRasterYSize(ds);
    summary->nXl = GDALGetRasterXSize(ds);
    summary->validCells = validStr.toLongLong();
    summary->valueMin = vmin.toDouble();
    summary->valueMax = vmax.toDouble();
    summary->timeMs = tms.toDouble();
    summary->sampleIntervalMs = dt.toDouble();
    summary->startTimeMs = t0.toDouble();
    summary->sampleIndex = sidx.toInt();
  }
  GDALClose(ds);
  return ok;
}

} // namespace paleo::sattr
