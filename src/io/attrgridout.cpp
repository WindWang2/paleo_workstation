// 层：数据
#include "io/attrgridout.h"
#include "ioerrors_internal.h"
#include "algorithms/projectgeotiff.h"
#include "io/gdalreg_internal.h" // P2：进程内幂等注册（writer 跑在任务 worker 线程，
                                 // 裸 GDALAllRegister 非线程安全——并发扫描会
                                 // 在 GDALRegister_* 里 SIGSEGV）
#include "metadata/atomicfile.h" // P2/#1024：paleoReplaceFile/paleoSyncDir

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>

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

using paleo::io_detail::setError;
using paleo::io_detail::ensureGdalRegistered; // P2：进程内幂等 GDAL 注册

constexpr float kNoData = -9999.f;

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

// P2/#1024：本次调用独占的暂存名——同目录（rename 原子）、点前缀隐藏
//（QDir 默认列举不含隐藏文件，产物扫描不误收）、带 pid+随机数（同 key
// 并发请求互不覆盖对方的暂存）。旧固定 .partial 名两请求并发互踩。
// 后缀沿用 metadata/atomicfile.h PaleoAtomicTransaction::recover 的
// `.{name}.tmp.*` 孤儿 glob——硬崩溃残留的暂存可被既有恢复机制清扫。
QString uniqueStagingPath(const QString &target)
{
  const QFileInfo info(target);
  return QStringLiteral("%1/.%2.tmp.%3-%4")
      .arg(info.absolutePath(), info.fileName())
      .arg(QCoreApplication::applicationPid())
      .arg(QRandomGenerator::global()->generate64(), 16, 16, QChar('0'));
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
  for (int i = 0; i < 6; ++i)
    if (!std::isfinite(affine[i]))
    {
      setError(error, QStringLiteral("地理参考仿射含非有限值（分量 %1）").arg(i));
      return false;
    }
  // P2/W4：时间/统计元数据前置验——非有限值或越界计数一旦落盘（"nan"），
  // 命中侧的严格 summary 解析会永远判 miss；在写入前如实失败。
  if (grid.sampleIndex < 0 || grid.validCells < 0 ||
      grid.validCells > qint64(grid.nIl) * grid.nXl ||
      !std::isfinite(grid.timeMs) || !std::isfinite(grid.sampleIntervalMs) ||
      grid.sampleIntervalMs < 0.0 || !std::isfinite(grid.startTimeMs) ||
      !std::isfinite(grid.valueMin) || !std::isfinite(grid.valueMax) ||
      grid.valueMin > grid.valueMax)
  {
    setError(error, QStringLiteral("时间切片时间/统计元数据非有限或越界"));
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
  // P2/#1024：绝不直接在正式路径上 GDALCreate——任何失败（创建/元数据/
  // 像元/关闭）只清理本次独占的暂存，旧有效缓存字节不变；全部写出并
  // 重开校验通过后才 paleoReplaceFile 发布。
  const QString staging = uniqueStagingPath(path);
  const auto failStaging = [&staging](const QString &what) {
    QFile::remove(staging); // 只删自己拥有的暂存，不碰正式路径
    return what;
  };
  ensureGdalRegistered();
  GDALDriverH drv = ProjectGeoTiff::driver();
  if (!drv)
  {
    setError(error, QStringLiteral("GTiff driver 不可用"));
    return false;
  }
  GDALDatasetH ds = ProjectGeoTiff::create(staging, grid.nXl, grid.nIl, GDT_Float32, drv);
  if (!ds)
  {
    setError(error, failStaging(QStringLiteral("无法创建暂存栅格：%1")
                                    .arg(QString::fromUtf8(CPLGetLastErrorMsg()))));
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
    setError(error, failStaging(QStringLiteral("栅格元数据写入失败：%1%2")
                                     .arg(path, detail.isEmpty() ? QString()
                                                                 : QStringLiteral(": ") + detail)));
    return false;
  }

  // 局部直角米测网 CRS（horizonbinner 同约定）：无大地基准的 ENGCRS。
  // 层树条目承诺「一定地理参考」——CRS 构造失败视为写失败（宁可不出
  // 栅格，不出会让 QGIS 弹 CRS 询问的无投影假栅格）。
  // P2/#1025：常量 WKT 也带 SET_FROM_USER_INPUT_LIMITATIONS（统一策略，
  // 禁文件/网络访问——该常量是内联 WKT，不受限影响）。
  OGRSpatialReference srs;
  char *wkt = nullptr;
  if (srs.SetFromUserInput(DataCatalog::localGridCrsWkt().toUtf8().constData(),
                           OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) !=
          OGRERR_NONE ||
      srs.exportToWkt(&wkt) != OGRERR_NONE || !wkt)
  {
    CPLFree(wkt);
    GDALClose(ds);
    setError(error, failStaging(QStringLiteral("局部测网 CRS 构造失败：%1").arg(path)));
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
      setError(error, failStaging(QStringLiteral("栅格像元写入失败（行 %1）：%2")
                                      .arg(r).arg(path)));
      return false;
    }
  }
  if (GDALClose(ds) != CE_None)
  {
    setError(error, failStaging(QStringLiteral("栅格落盘失败：%1").arg(path)));
    return false;
  }

  // P2/W4：发布前重开校验（真实 GDAL 读回）——尺寸、身份元数据、
  // geotransform 与首/末行像素逐值核对；半成品在替换正式文件之前拦下。
  {
    GDALDatasetH check = GDALOpen(staging.toUtf8().constData(), GA_ReadOnly);
    if (!check)
    {
      setError(error, failStaging(QStringLiteral("暂存栅格重开失败：%1").arg(staging)));
      return false;
    }
    bool ok = GDALGetRasterXSize(check) == grid.nXl &&
              GDALGetRasterYSize(check) == grid.nIl;
    const CSLConstList meta = GDALGetMetadata(check, nullptr);
    const char *hash = CSLFetchNameValue(meta, "PALEO_PARAM_HASH");
    const char *attr = CSLFetchNameValue(meta, "PALEO_ATTR_ID");
    const char *sidx = CSLFetchNameValue(meta, "PALEO_TIME_SAMPLE_INDEX");
    ok = ok && hash && attr && sidx &&
         QString::fromUtf8(hash) == grid.paramHash &&
         QString::fromUtf8(attr) == grid.attrId &&
         QByteArray::number(qint64(grid.sampleIndex)) == QByteArray(sidx);
    double gtCheck[6] = {0, 0, 0, 0, 0, 0};
    if (ok && GDALGetGeoTransform(check, gtCheck) != CE_None)
      ok = false;
    for (int i = 0; ok && i < 6; ++i)
      if (!std::isfinite(gtCheck[i]) || gtCheck[i] != gt[i])
        ok = false;
    std::vector<float> rowCheck(std::size_t(grid.nXl), 0.f);
    const int rowsToCheck[] = {0, grid.nIl - 1};
    for (int ri = 0; ok && ri < 2; ++ri)
    {
      const int r = rowsToCheck[ri];
      if (GDALRasterIO(GDALGetRasterBand(check, 1), GF_Read, 0, r, grid.nXl, 1,
                       rowCheck.data(), grid.nXl, 1, GDT_Float32, 0, 0) != CE_None)
      {
        ok = false;
        break;
      }
      for (int x = 0; x < grid.nXl; ++x)
      {
        const float expect =
            std::isnan(grid.values[std::size_t(r) * grid.nXl + x])
                ? kNoData
                : grid.values[std::size_t(r) * grid.nXl + x];
        if (rowCheck[std::size_t(x)] != expect)
        {
          ok = false;
          break;
        }
      }
    }
    GDALClose(check);
    if (!ok)
    {
      setError(error, failStaging(
                          QStringLiteral("暂存栅格校验失败（尺寸/身份元数据/像素不符）：%1")
                              .arg(path)));
      return false;
    }
  }

  if (!paleoReplaceFile(staging, path))
  {
    setError(error, failStaging(QStringLiteral("缓存栅格发布失败（rename）：%1").arg(path)));
    return false;
  }
  // rename 目录项落盘（atomicfile 契约）；失败只告警不阻断——数据已
  // 由文件级 fsync 持久，掉电极端窗口回退旧文件仍是一致缓存。
  if (!paleoSyncDir(dir.absolutePath()))
    qWarning("time-slice GeoTIFF: parent directory fsync failed for %s",
             qUtf8Printable(dir.absolutePath()));
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
  ensureGdalRegistered();
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
  // 三个时间键与身份键同口径：缺键即判缓存不可用——否则 tms/dt/t0 静默
  // 回填 0.0，缓存摘要的时间域是错值（缺元数据键→视同未命中重算）。
  if (attrId.isEmpty() || hash.isEmpty() || validStr.isEmpty() ||
      vmin.isEmpty() || vmax.isEmpty() || sidx.isEmpty() || tms.isEmpty() ||
      dt.isEmpty() || t0.isEmpty())
  {
    ok = false;
    setError(error, QStringLiteral("缓存栅格缺 PALEO_* 身份元数据：%1")
                             .arg(path));
  }
  // P2/W5：数值键逐个严格解析——垃圾串（"abc"/"nan"/超范围）不得被
  // toInt/toDouble 静默变 0 后当真值交给下游统计面板。
  qint64 validCells = 0;
  double valueMin = 0.0, valueMax = 0.0, timeMs = 0.0, sampleIntervalMs = 0.0,
         startTimeMs = 0.0;
  int sampleIndex = 0;
  if (ok)
  {
    bool conv = true;
    validCells = validStr.toLongLong(&conv);
    ok = conv;
    valueMin = vmin.toDouble(&conv);
    ok = ok && conv;
    valueMax = vmax.toDouble(&conv);
    ok = ok && conv;
    timeMs = tms.toDouble(&conv);
    ok = ok && conv;
    sampleIntervalMs = dt.toDouble(&conv);
    ok = ok && conv;
    startTimeMs = t0.toDouble(&conv);
    ok = ok && conv;
    sampleIndex = sidx.toInt(&conv);
    ok = ok && conv;
    if (!ok)
      setError(error, QStringLiteral("缓存栅格数值元数据不可解析：%1").arg(path));
  }
  double gt[6] = {0, 0, 0, 0, 0, 0};
  if (ok && GDALGetGeoTransform(ds, gt) != CE_None)
  {
    ok = false;
    setError(error, QStringLiteral("缓存栅格地理参考缺失：%1").arg(path));
  }
  if (ok)
  {
    summary->nIl = GDALGetRasterYSize(ds);
    summary->nXl = GDALGetRasterXSize(ds);
    if (summary->nIl <= 0 || summary->nXl <= 0 ||
        validCells < 0 ||
        validCells > qint64(summary->nIl) * summary->nXl ||
        sampleIndex < 0 || sampleIntervalMs < 0.0 ||
        !std::isfinite(valueMin) || !std::isfinite(valueMax) ||
        valueMin > valueMax || !std::isfinite(timeMs) ||
        !std::isfinite(sampleIntervalMs) || !std::isfinite(startTimeMs))
    {
      ok = false;
      setError(error, QStringLiteral("缓存栅格元数据数值越界/非有限：%1").arg(path));
    }
    for (int i = 0; ok && i < 6; ++i)
      if (!std::isfinite(gt[i]))
      {
        ok = false;
        setError(error, QStringLiteral("缓存栅格 geotransform 非有限：%1").arg(path));
      }
  }
  if (ok)
  {
    summary->attrId = attrId;
    summary->paramHash = hash;
    summary->validCells = validCells;
    summary->valueMin = valueMin;
    summary->valueMax = valueMax;
    summary->timeMs = timeMs;
    summary->sampleIntervalMs = sampleIntervalMs;
    summary->startTimeMs = startTimeMs;
    summary->sampleIndex = sampleIndex;
  }
  GDALClose(ds);
  return ok;
}

} // namespace paleo::sattr
