#include "horizonbinner.h"

#include "../catalog/datacatalog.h"

#include <cpl_conv.h>
#include <cpl_error.h>
#include <gdal.h>
#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <QDir>
#include <QFile>
#include <QRegularExpression>

#include <cmath>
#include <limits>

namespace
{
  const float kNoData = -9999.0f;

  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  double grabDouble(const QString &line, int index)
  {
    // P 行是逗号分隔："# P1:      1315,      4165,     0.00000,     0.00000"
    const QStringList t = QString(line).replace(QLatin1Char(','), QLatin1Char(' '))
                          .split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    return index < t.size() ? t.at(index).toDouble() : 0.0;
  }
} // namespace

bool parseHorizonHeader(const QByteArray &text, HorizonHeader *out, QString *error)
{
  HorizonHeader h;
  const QString data = QString::fromUtf8(text);
  for (const QString &raw : data.split(QRegularExpression(QStringLiteral("[\r\n]")),
                                        Qt::SkipEmptyParts))
  {
    const QString line = raw.trimmed();
    if (!line.startsWith(QLatin1Char('#')))
      break; // 头部结束（数据行开始）
    if (line.startsWith(QStringLiteral("# Grid_size:"), Qt::CaseInsensitive))
    {
      const QString rest = line.mid(12); // "411x641# Survey..."
      const QRegularExpression re(QStringLiteral("(\\d+)\\s*x\\s*(\\d+)"),
                                  QRegularExpression::CaseInsensitiveOption);
      const auto m = re.match(rest);
      if (m.hasMatch())
      {
        h.gridRows = m.captured(1).toInt();
        h.gridCols = m.captured(2).toInt();
      }
    }
    else if (line.startsWith(QStringLiteral("# P1:"), Qt::CaseInsensitive))
    {
      // 归一化后 token：[0]='#' [1]="P1:" [2]=inline [3]=xline [4]=x [5]=y
      h.p1Inline = qRound(grabDouble(line, 2));
      h.p1Xline = qRound(grabDouble(line, 3));
      h.p1x = grabDouble(line, 4);
      h.p1y = grabDouble(line, 5);
    }
    else if (line.startsWith(QStringLiteral("# P2:"), Qt::CaseInsensitive))
    {
      h.p2Inline = qRound(grabDouble(line, 2));
      h.p2Xline = qRound(grabDouble(line, 3));
      h.p2x = grabDouble(line, 4);
      h.p2y = grabDouble(line, 5);
    }
    else if (line.startsWith(QStringLiteral("# P3:"), Qt::CaseInsensitive))
    {
      h.p3Inline = qRound(grabDouble(line, 2));
      h.p3Xline = qRound(grabDouble(line, 3));
      h.p3x = grabDouble(line, 4);
      h.p3y = grabDouble(line, 5);
    }
    else if (line.startsWith(QStringLiteral("# Z_units:"), Qt::CaseInsensitive))
    {
      h.zUnits = line.mid(10).trimmed();
    }
  }
  if (h.gridRows <= 0 || h.gridCols <= 0)
  {
    setError(error, QStringLiteral("horizon header lacks Grid_size"));
    return false;
  }
  if (out)
    *out = h;
  return true;
}

bool binHorizon(const QByteArray &text, BinnedHorizon *out, QString *error)
{
  HorizonHeader h;
  if (!parseHorizonHeader(text, &h, error))
    return false;

  BinnedHorizon b;
  b.rows = h.gridRows;
  b.cols = h.gridCols;
  b.z.fill(kNoData, b.rows * b.cols);
  b.dx = (h.p2x - h.p1x) / (h.gridCols - 1);
  b.dy = (h.p3y - h.p2y) / (h.gridRows - 1);
  // 北向上：像元 (0,0) 在左上=最大 y；行号随 inline 增加而 y 减小。
  b.originX = h.p1x;
  b.originY = h.p1y + (h.gridRows - 1) * b.dy;

  QVector<bool> filled(b.rows * b.cols, false);

  const QString data = QString::fromUtf8(text);
  for (const QString &raw : data.split(QRegularExpression(QStringLiteral("[\r\n]")),
                                        Qt::SkipEmptyParts))
  {
    const QString line = raw.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
      continue;
    const QStringList t = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (t.size() < 5)
      continue;
    bool okInl = false, okXl = false, okZ = false;
    const int inl = t.at(3).toInt(&okInl);
    const int xl = t.at(4).toInt(&okXl);
    const float z = t.at(2).toFloat(&okZ);
    if (!okInl || !okXl || !okZ)
      continue;
    // 北向上：行 0 是最大 y（inline 最大侧）；inline=p1Inline 落在末行。
    const int row = (h.gridRows - 1) - (inl - h.p1Inline);
    const int col = xl - h.p1Xline;
    if (row < 0 || row >= b.rows || col < 0 || col >= b.cols)
    {
      ++b.rejected; // 越界的点不写入，只计入拒绝数（§3）
      continue;
    }
    const int idx = row * b.cols + col;
    if (filled.at(idx))
      ++b.collisions; // 同像元多点：保留最后一点（下方覆盖）
    else
    {
      filled[idx] = true;
      ++b.filledCells;
    }
    b.z[idx] = z;
  }
  // z 范围从最终栅格取（被覆盖的点不参与）。
  float zMin = std::numeric_limits<float>::max();
  float zMax = std::numeric_limits<float>::lowest();
  for (const float v : b.z)
  {
    if (v == kNoData)
      continue;
    if (v < zMin)
      zMin = v;
    if (v > zMax)
      zMax = v;
  }
  b.zMin = b.filledCells ? zMin : 0.0;
  b.zMax = b.filledCells ? zMax : 0.0;
  if (out)
    *out = b;
  return true;
}

bool writeHorizonGeoTiff(const BinnedHorizon &b, const QString &destPath, QString *error)
{
  const QDir dir = QFileInfo(destPath).absoluteDir();
  if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
  {
    setError(error, QStringLiteral("cannot create directory %1").arg(dir.absolutePath()));
    return false;
  }
  GDALAllRegister();
  GDALDriverH drv = GDALGetDriverByName("GTiff");
  if (!drv)
  {
    setError(error, QStringLiteral("GTiff driver unavailable"));
    return false;
  }
  GDALDatasetH ds = GDALCreate(drv, destPath.toUtf8().constData(), b.cols, b.rows, 1, GDT_Float32,
                               nullptr);
  if (!ds)
  {
    setError(error, QStringLiteral("cannot create %1: %2").arg(destPath, QString::fromUtf8(CPLGetLastErrorMsg())));
    return false;
  }

  double gt[6] = {b.originX, b.dx, 0.0, b.originY, 0.0, -b.dy};
  GDALSetGeoTransform(ds, gt);
  GDALSetRasterNoDataValue(GDALGetRasterBand(ds, 1), kNoData);

  // 局部直角米测网（§3）——无大地基准的 ENGCRS，非 EPSG:4326。GDAL 接受该
  // WKT 并以 LOCAL_CS 落盘；SetFromUserInput 失败时宁可让 GeoTIFF SRS 留空，
  // 也绝不把局部米写成经纬度。
  OGRSpatialReference srs;
  if (srs.SetFromUserInput(DataCatalog::localGridCrsWkt().toUtf8().constData()) == OGRERR_NONE)
  {
    char *wkt = nullptr;
    if (srs.exportToWkt(&wkt) == OGRERR_NONE && wkt)
      GDALSetProjection(ds, wkt);
    CPLFree(wkt);
  }

  const CPLErr err = GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, 0, b.cols, b.rows,
                                  const_cast<float *>(b.z.constData()), b.cols, b.rows, GDT_Float32,
                                  0, 0);
  GDALClose(ds);
  if (err != CE_None)
  {
    setError(error, QStringLiteral("raster write failed for %1").arg(destPath));
    return false;
  }
  return true;
}
