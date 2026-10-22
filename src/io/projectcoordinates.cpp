// 层：数据
#include "projectcoordinates.h"
#include <QFile>
#include <QtEndian>
#include <ogr_spatialref.h>
#include <gdal_utils.h>
#include <cmath>
#include <memory>
namespace {
using Transform = std::unique_ptr<OGRCoordinateTransformation, decltype(&OCTDestroyCoordinateTransformation)>;
Transform transformation(const QString &from, const QString &to) {
  OGRSpatialReference a, b;
  if (a.SetFromUserInput(from.toUtf8().constData()) != OGRERR_NONE ||
      b.SetFromUserInput(to.toUtf8().constData()) != OGRERR_NONE) return {nullptr, OCTDestroyCoordinateTransformation};
  a.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER); b.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  return {OGRCreateCoordinateTransformation(&a, &b), OCTDestroyCoordinateTransformation};
}
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
bool transformHeader(QByteArray &header, OGRCoordinateTransformation *ct, bool geographic) {
  const qint16 scalar = qFromBigEndian<qint16>(header.constData() + 70);
  const qint16 units = qFromBigEndian<qint16>(header.constData() + 88);
  if ((geographic && units != 0 && units != 2 && units != 3) ||
      (!geographic && units != 0 && units != 1)) return false;
  const double scale = (scalar < 0 ? -1. / scalar : scalar > 0 ? scalar : 1.) /
                       (geographic && units == 2 ? 3600. : 1.);
  for (int offset : {72, 80, 180}) {
    double x = qFromBigEndian<qint32>(header.constData() + offset) * scale;
    double y = qFromBigEndian<qint32>(header.constData() + offset + 4) * scale;
    if (offset != 180 && x == 0 && y == 0) continue; // Unassigned acquisition coordinates stay unassigned.
    if (!ct->Transform(1, &x, &y) || !std::isfinite(x) || !std::isfinite(y) ||
        std::abs(x * 100) > 2147483647. || std::abs(y * 100) > 2147483647.) return false;
    qToBigEndian<qint32>(qint32(std::llround(x * 100)), header.data() + offset);
    qToBigEndian<qint32>(qint32(std::llround(y * 100)), header.data() + offset + 4);
  }
  qToBigEndian<qint16>(-100, header.data() + 70);
  qToBigEndian<qint16>(1, header.data() + 88);
  return true;
}
}
namespace paleo::io {
bool isMetricProjectCrs(const QString &crs) {
  OGRSpatialReference srs;
  return srs.SetFromUserInput(crs.toUtf8().constData()) == OGRERR_NONE && srs.IsProjected()
      && std::abs(srs.GetLinearUnits() - 1.) < 1e-9;
}
QString coordinateWkt(const QString &crs) {
  OGRSpatialReference srs; char *wkt = nullptr;
  if (srs.SetFromUserInput(crs.toUtf8().constData()) != OGRERR_NONE || srs.exportToWkt(&wkt) != OGRERR_NONE) return {};
  const auto result = QString::fromUtf8(wkt); CPLFree(wkt); return result;
}
bool projectRasterFile(const QString &path, const QString &target, QString *error, const QString &source) {
  GDALAllRegister();
  if (!source.isEmpty()) {
    auto declared = GDALOpen(path.toUtf8().constData(), GA_Update);
    const auto wkt = coordinateWkt(source).toUtf8();
    if (!declared || wkt.isEmpty()) { if (declared) GDALClose(declared); return fail(error, QStringLiteral("栅格来源坐标系无效")); }
    const bool ok = GDALSetProjection(declared, wkt.constData()) == CE_None &&
                    GDALSetMetadataItem(declared, "PALEO_CRS_WKT", wkt.constData(), nullptr) == CE_None;
    GDALClose(declared); if (!ok) return fail(error, QStringLiteral("无法保存来源坐标声明"));
  }
  if (target.isEmpty()) return true;
  auto input = GDALOpen(path.toUtf8().constData(), GA_ReadOnly);
  if (!input) return fail(error, QStringLiteral("无法读取待转换栅格"));
  OGRSpatialReference from, to;
  if (from.SetFromUserInput(GDALGetProjectionRef(input)) != OGRERR_NONE ||
      to.SetFromUserInput(target.toUtf8().constData()) != OGRERR_NONE) {
    GDALClose(input); return fail(error, QStringLiteral("栅格来源或工程坐标系无效"));
  }
  from.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER); to.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  if (from.IsSame(&to)) { GDALClose(input); return true; }
  const auto destination = path + QStringLiteral(".projected.tif");
  QByteArray encoded = target.toUtf8();
  char *args[] = {const_cast<char *>("-t_srs"), encoded.data(), const_cast<char *>("-r"),
                  const_cast<char *>("bilinear"), nullptr};
  auto *options = GDALWarpAppOptionsNew(args, nullptr);
  int usage = 0;
  const auto output = GDALWarp(destination.toUtf8().constData(), nullptr, 1,
                               &input, options, &usage);
  GDALWarpAppOptionsFree(options); GDALClose(input);
  if (!output) { QFile::remove(destination); return fail(error, QStringLiteral("栅格转换到工程基准坐标系失败")); }
  GDALSetMetadataItem(output, "PALEO_CRS_WKT", coordinateWkt(target).toUtf8().constData(), nullptr);
  GDALClose(output);
  // Both files are private staged outputs; never replace external source files.
  if (!QFile::remove(path) || !QFile::rename(destination, path)) return fail(error, QStringLiteral("无法保存坐标转换栅格"));
  return true;
}
bool projectSegyFile(const QString &source, const QString &output, const QString &from,
                     const QString &target, QString *error) {
  auto ct = transformation(from, target);
  if (!ct) return fail(error, QStringLiteral("无法建立地震坐标转换"));
  OGRSpatialReference sourceReference;
  sourceReference.SetFromUserInput(from.toUtf8().constData());
  QFile input(source);
  if (!input.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("无法读取地震源文件"));
  const auto header = input.read(3600);
  if (header.size() != 3600) return fail(error, QStringLiteral("地震头不完整"));
  const int format = qFromBigEndian<quint16>(header.constData() + 3224);
  const int samples = qFromBigEndian<quint16>(header.constData() + 3220);
  const int bytes = (format == 1 || format == 2 || format == 5) ? 4 : format == 3 ? 2 : format == 8 ? 1 : 0;
  if (!bytes || !samples || qFromBigEndian<qint16>(header.constData() + 3504) != 0)
    return fail(error, QStringLiteral("当前地震格式不能安全转换坐标，请提供固定道长 SEG-Y"));
  const qint64 stride = 240 + samples * bytes;
  if ((input.size() - 3600) % stride) return fail(error, QStringLiteral("地震道长不一致，已拒绝坐标转换"));
  if (!QFile::copy(source, output)) return fail(error, QStringLiteral("无法创建地震坐标转换副本"));
  QFile::setPermissions(output, QFile::permissions(output) | QFileDevice::WriteOwner);
  QFile copy(output);
  if (!copy.open(QIODevice::ReadWrite)) return fail(error, QStringLiteral("无法写入地震副本"));
  for (qint64 offset = 3600; offset < copy.size(); offset += stride) {
    if (!copy.seek(offset)) return fail(error, QStringLiteral("无法定位地震道头"));
    auto trace = copy.read(240);
    const int ns = trace.size() == 240 ? qFromBigEndian<quint16>(trace.constData() + 114) : -1;
    if ((ns && ns != samples) || !transformHeader(trace, ct.get(), sourceReference.IsGeographic()) || !copy.seek(offset) || copy.write(trace) != 240)
      return fail(error, QStringLiteral("地震坐标转换失败；未发布半成品"));
  }
  for (int line = 0; line < 40; ++line) {
    if (line != 38 && !header.mid(line * 80, 80).toUpper().contains("EPSG")) continue;
    const auto text = QStringLiteral("C%1 CRS %2; COORDINATES TRANSFORMED; SAMPLES UNCHANGED")
        .arg(line + 1, 2, 10, QLatin1Char('0')).arg(target).toLatin1().left(80).leftJustified(80, ' ');
    if (!copy.seek(line * 80) || copy.write(text) != 80)
      return fail(error, QStringLiteral("无法保存地震坐标声明"));
  }
  return true;
}
}
