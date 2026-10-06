// 层：数据
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

  bool grabDouble(const QString &line, int index, double *out)
  {
    // P 行是逗号分隔："# P1:      1315,      4165,     0.00000,     0.00000"
    const QStringList t = QString(line).replace(QLatin1Char(','), QLatin1Char(' '))
                          .split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (index >= t.size())
      return false;
    bool ok = false;
    const double value = t.at(index).toDouble(&ok);
    if (!ok || !std::isfinite(value))
      return false;
    *out = value;
    return true;
  }

  bool grabInt(const QString &line, int index, int *out)
  {
    const QStringList t = QString(line).replace(QLatin1Char(','), QLatin1Char(' '))
                          .split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (index >= t.size())
      return false;
    bool ok = false;
    const double value = t.at(index).toDouble(&ok);
    if (!ok || !std::isfinite(value) || std::trunc(value) != value ||
        value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
      return false;
    *out = static_cast<int>(value);
    return true;
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
        bool rowsOk = false, colsOk = false;
        h.gridRows = m.captured(1).toInt(&rowsOk);
        h.gridCols = m.captured(2).toInt(&colsOk);
        if (!rowsOk || !colsOk)
        {
          setError(error, QStringLiteral("horizon header has invalid Grid_size"));
          return false;
        }
      }
    }
    else if (line.startsWith(QStringLiteral("# P1:"), Qt::CaseInsensitive))
    {
      // 归一化后 token：[0]='#' [1]="P1:" [2]=inline [3]=xline [4]=x [5]=y
      if (!grabInt(line, 2, &h.p1Inline) || !grabInt(line, 3, &h.p1Xline) ||
          !grabDouble(line, 4, &h.p1x) || !grabDouble(line, 5, &h.p1y))
      {
        setError(error, QStringLiteral("horizon header has invalid P1 geometry"));
        return false;
      }
      h.hasP1 = true;
    }
    else if (line.startsWith(QStringLiteral("# P2:"), Qt::CaseInsensitive))
    {
      if (!grabInt(line, 2, &h.p2Inline) || !grabInt(line, 3, &h.p2Xline) ||
          !grabDouble(line, 4, &h.p2x) || !grabDouble(line, 5, &h.p2y))
      {
        setError(error, QStringLiteral("horizon header has invalid P2 geometry"));
        return false;
      }
      h.hasP2 = true;
    }
    else if (line.startsWith(QStringLiteral("# P3:"), Qt::CaseInsensitive))
    {
      if (!grabInt(line, 2, &h.p3Inline) || !grabInt(line, 3, &h.p3Xline) ||
          !grabDouble(line, 4, &h.p3x) || !grabDouble(line, 5, &h.p3y))
      {
        setError(error, QStringLiteral("horizon header has invalid P3 geometry"));
        return false;
      }
      h.hasP3 = true;
    }
    else if (line.startsWith(QStringLiteral("# Z_units:"), Qt::CaseInsensitive))
    {
      h.zUnits = line.mid(10).trimmed();
    }
  }
  if (h.gridRows < 2 || h.gridCols < 2)
  {
    setError(error, QStringLiteral("horizon header requires Grid_size of at least 2x2"));
    return false;
  }
  const qint64 inlineMax = static_cast<qint64>(h.p1Inline) + h.gridRows - 1;
  const qint64 xlineMax = static_cast<qint64>(h.p1Xline) + h.gridCols - 1;
  const qint64 cellCount = static_cast<qint64>(h.gridRows) * h.gridCols;
  constexpr qint64 kMaxCellCount = 100'000'000;
  if (inlineMax > std::numeric_limits<int>::max() ||
      xlineMax > std::numeric_limits<int>::max() ||
      cellCount > kMaxCellCount ||
      cellCount > std::numeric_limits<int>::max())
  {
    setError(error, QStringLiteral("horizon Grid_size or line range exceeds supported limits"));
    return false;
  }
  if (!h.hasP1 || !h.hasP2 || !h.hasP3)
  {
    setError(error, QStringLiteral("horizon header lacks P1/P2/P3 geometry"));
    return false;
  }
  if (h.p1Inline != h.p2Inline || h.p2Xline != h.p3Xline ||
      h.p3Inline != h.p1Inline + h.gridRows - 1 ||
      h.p2Xline != h.p1Xline + h.gridCols - 1 ||
      h.p2x == h.p1x || h.p3y == h.p2y)
  {
    setError(error, QStringLiteral("horizon header P1/P2/P3 do not match Grid_size"));
    return false;
  }
  // 角点号域须与 Grid_size 自洽：inline 跨度=行数−1、xline 跨度=列数−1，
  // 且 P1 是 (min inline, min xline) 原点角——binHorizon 的行/列式依赖它。
  const int inMin = qMin(h.p1Inline, qMin(h.p2Inline, h.p3Inline));
  const int inMax = qMax(h.p1Inline, qMax(h.p2Inline, h.p3Inline));
  const int xlMin = qMin(h.p1Xline, qMin(h.p2Xline, h.p3Xline));
  const int xlMax = qMax(h.p1Xline, qMax(h.p2Xline, h.p3Xline));
  if (inMax - inMin != h.gridRows - 1 || xlMax - xlMin != h.gridCols - 1 ||
      h.p1Inline != inMin || h.p1Xline != xlMin)
  {
    setError(error,
             QStringLiteral("horizon P-corner survey range disagrees with Grid_size"));
    return false;
  }
  // 数值几何：坐标有限，x 随 xline、y 随 inline 单调增（本工区约定），
  // 像元尺寸由此非零——dx=0/dy=0 的退化网格不该落盘。
  if (!std::isfinite(h.p1x) || !std::isfinite(h.p1y) || !std::isfinite(h.p2x) ||
      !std::isfinite(h.p2y) || !std::isfinite(h.p3x) || !std::isfinite(h.p3y) ||
      h.p2x <= h.p1x || h.p3y <= h.p2y)
  {
    setError(error, QStringLiteral("horizon P-corner geometry is degenerate"));
    return false;
  }
  if (std::abs(h.p2y - h.p1y) > 1e-3 || std::abs(h.p3x - h.p2x) > 1e-3)
  {
    setError(error, QStringLiteral("horizon grid is rotated or non-orthogonal"));
    return false;
  }
  if (out)
    *out = h;
  return true;
}

HorizonScatter parseHorizonScatter(const QByteArray &text)
{
  HorizonScatter out;
  bool ilSeen = false, xlSeen = false;
  const QString data = QString::fromUtf8(text);
  for (const QString &raw : data.split(QRegularExpression(QStringLiteral("[\r\n]")),
                                        Qt::SkipEmptyParts))
  {
    const QString line = raw.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
    {
      ++out.skipped;
      continue;
    }
    const QStringList t =
        line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (t.size() < 3)
    {
      ++out.skipped;
      continue;
    }
    bool okX = false, okY = false, okZ = false;
    const double x = t.at(0).toDouble(&okX);
    const double y = t.at(1).toDouble(&okY);
    const float z = t.at(2).toFloat(&okZ);
    if (!okX || !okY || !okZ || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z) || z == kNoData)
    {
      ++out.skipped;
      continue;
    }
    HorizonScatterPoint p;
    p.x = x;
    p.y = y;
    p.z = z;
    out.points.push_back(p);
    if (t.size() >= 5)
    {
      bool okInl = false, okXl = false;
      const int inl = t.at(3).toInt(&okInl);
      const int xl = t.at(4).toInt(&okXl);
      if (okInl && okXl)
      {
        if (!ilSeen)
        {
          out.inlineMin = out.inlineMax = inl;
          ilSeen = true;
        }
        else
        {
          out.inlineMin = qMin(out.inlineMin, inl);
          out.inlineMax = qMax(out.inlineMax, inl);
        }
        if (!xlSeen)
        {
          out.xlineMin = out.xlineMax = xl;
          xlSeen = true;
        }
        else
        {
          out.xlineMin = qMin(out.xlineMin, xl);
          out.xlineMax = qMax(out.xlineMax, xl);
        }
      }
    }
  }
  out.hasInlineRange = ilSeen;
  out.hasXlineRange = xlSeen;
  return out;
}

bool binHorizon(const QByteArray &text, BinnedHorizon *out, QString *error)
{
  HorizonHeader h;
  if (!parseHorizonHeader(text, &h, error))
    return false;

  BinnedHorizon b;
  b.rows = h.gridRows;
  b.cols = h.gridCols;
  const int cellCount = static_cast<int>(static_cast<qint64>(b.rows) * b.cols);
  b.dx = (h.p2x - h.p1x) / (h.gridCols - 1);
  b.dy = (h.p3y - h.p2y) / (h.gridRows - 1);
  b.originX = h.p1x;
  b.originY = h.p1y + (h.gridRows - 1) * b.dy;
  if (!(b.dx > 0.0) || !(b.dy > 0.0) || !std::isfinite(b.dx) || !std::isfinite(b.dy) ||
      !std::isfinite(b.originX) || !std::isfinite(b.originY))
  {
    setError(error, QStringLiteral("horizon header has invalid grid spacing"));
    return false;
  }
  b.z.fill(kNoData, cellCount);
  if (h.hasP1)
  {
    b.hasInlineRange = true;
    b.inlineMin = h.p1Inline;
    b.inlineMax = h.p1Inline + h.gridRows - 1;
    b.hasXlineRange = true;
    b.xlineMin = h.p1Xline;
    b.xlineMax = h.p1Xline + h.gridCols - 1;
  }
  // 北向上：像元 (0,0) 在左上=最大 y；行号随 inline 增加而 y 减小。

  QVector<bool> filled(cellCount, false);

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
    if (!okInl || !okXl || !okZ || !std::isfinite(z) || z == kNoData)
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

  const auto setIntMetadata = [ds](const char *key, int value) {
    const QByteArray encoded = QByteArray::number(value);
    return GDALSetMetadataItem(ds, key, encoded.constData(), nullptr) == CE_None;
  };
  const auto setDblMetadata = [ds](const char *key, double value) {
    const QByteArray encoded = QByteArray::number(value);
    return GDALSetMetadataItem(ds, key, encoded.constData(), nullptr) == CE_None;
  };
  // T21/audit #38：测网号域与装箱计数写进栅格元数据——验证残差行要由
  // PALEO_INLINE_* 把采样点 x 反算成 inline 去开剖面。PALEO_DT_MS/PALEO_T0_MS
  // 来自 SEG-Y、散点文本里没有，只在调用方给了有限值时才写，不编值。
  if ((b.hasInlineRange &&
       (!setIntMetadata("PALEO_INLINE_MIN", b.inlineMin) ||
        !setIntMetadata("PALEO_INLINE_MAX", b.inlineMax))) ||
      (b.hasXlineRange &&
       (!setIntMetadata("PALEO_XLINE_MIN", b.xlineMin) ||
        !setIntMetadata("PALEO_XLINE_MAX", b.xlineMax))) ||
      !setIntMetadata("PALEO_COLLISIONS", b.collisions) ||
      !setIntMetadata("PALEO_REJECTED", b.rejected) ||
      !setIntMetadata("PALEO_FILLED_CELLS", b.filledCells) ||
      (std::isfinite(b.dtMs) && !setDblMetadata("PALEO_DT_MS", b.dtMs)) ||
      (std::isfinite(b.t0Ms) && !setDblMetadata("PALEO_T0_MS", b.t0Ms)))
  {
    const QString detail = QString::fromUtf8(CPLGetLastErrorMsg());
    GDALClose(ds);
    QFile::remove(destPath); // #233：失败不留无 PALEO_* 元数据的半成品栅格
    setError(error, QStringLiteral("cannot write raster metadata for %1%2")
                         .arg(destPath, detail.isEmpty() ? QString() : QStringLiteral(": ") + detail));
    return false;
  }

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
    QFile::remove(destPath); // #233：像元不全的残件不占缓存位
    setError(error, QStringLiteral("raster write failed for %1").arg(destPath));
    return false;
  }
  return true;
}
