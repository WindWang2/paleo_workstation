// 层：数据
#include "projectcoordinates.h"
#include "../metadata/atomicfile.h" // paleoReplaceFile：发布用替换语义（不预删目标）
#include "segyreader.h"
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
  if (a.SetFromUserInput(from.toUtf8().constData(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE ||
      b.SetFromUserInput(to.toUtf8().constData(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE) return {nullptr, OCTDestroyCoordinateTransformation};
  a.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER); b.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  return {OGRCreateCoordinateTransformation(&a, &b), OCTDestroyCoordinateTransformation};
}
bool fail(QString *error, const QString &message) { if (error) *error = message; return false; }
qint16 rd16(const char *p, bool le) { return le ? qFromLittleEndian<qint16>(p) : qFromBigEndian<qint16>(p); }
quint16 rdU16(const char *p, bool le) { return le ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p); }
qint32 rd32(const char *p, bool le) { return le ? qFromLittleEndian<qint32>(p) : qFromBigEndian<qint32>(p); }
void wr16(qint16 v, char *p, bool le) { if (le) qToLittleEndian<qint16>(v, p); else qToBigEndian<qint16>(v, p); }
void wr32(qint32 v, char *p, bool le) { if (le) qToLittleEndian<qint32>(v, p); else qToBigEndian<qint32>(v, p); }
// #855/#857：按源文件字节序读写道头；目标为地理坐标系时写 rev1 units=2（弧秒，
// scalar -100 → 0.01″ ≈ 0.3 m），不再把度值当长度 ×100（精度仅 0.01° 且单位错标）。
bool transformHeader(QByteArray &header, OGRCoordinateTransformation *ct, bool geographic,
                     bool targetGeographic, bool le) {
  const qint16 scalar = rd16(header.constData() + 70, le);
  const qint16 units = rd16(header.constData() + 88, le);
  if ((geographic && units != 0 && units != 2 && units != 3) ||
      (!geographic && units != 0 && units != 1)) return false;
  const double scale = (scalar < 0 ? -1. / scalar : scalar > 0 ? scalar : 1.) /
                       (geographic && units == 2 ? 3600. : 1.);
  const double outFactor = targetGeographic ? 3600. * 100. : 100.;
  for (int offset : {72, 80, 180}) {
    double x = rd32(header.constData() + offset, le) * scale;
    double y = rd32(header.constData() + offset + 4, le) * scale;
    // #856：任何坐标对 (0,0) 都是未赋值（含 CDP 181/185），保持未赋值。
    if (x == 0 && y == 0) continue;
    if (!ct->Transform(1, &x, &y) || !std::isfinite(x) || !std::isfinite(y) ||
        std::abs(x * outFactor) > 2147483647. || std::abs(y * outFactor) > 2147483647.) return false;
    wr32(qint32(std::llround(x * outFactor)), header.data() + offset, le);
    wr32(qint32(std::llround(y * outFactor)), header.data() + offset + 4, le);
  }
  wr16(-100, header.data() + 70, le);
  wr16(targetGeographic ? 2 : 1, header.data() + 88, le);
  return true;
}
// IBM EBCDIC cp037 ⇄ Latin-1（SEG-Y 文本头可打印子集）。
const unsigned char kEbcdicToAscii[256] = {
  0,1,2,3,156,9,134,127,151,141,142,11,12,13,14,15,16,17,18,19,157,133,8,135,24,25,146,143,28,29,30,31,
  128,129,130,131,132,10,23,27,136,137,138,139,140,5,6,7,144,145,22,147,148,149,150,4,152,153,154,155,20,21,158,26,
  32,160,226,228,224,225,227,229,231,241,162,46,60,40,43,124,38,233,234,235,232,237,238,239,236,223,33,36,42,41,59,172,
  45,47,194,196,192,193,195,197,199,209,166,44,37,95,62,63,248,201,202,203,200,205,206,207,204,96,58,35,64,39,61,34,
  216,97,98,99,100,101,102,103,104,105,171,187,240,253,254,177,176,106,107,108,109,110,111,112,113,114,170,186,230,184,198,164,
  181,126,115,116,117,118,119,120,121,122,161,191,208,221,222,174,94,163,165,183,169,167,182,188,189,190,91,93,175,168,180,215,
  123,65,66,67,68,69,70,71,72,73,173,244,246,242,243,245,125,74,75,76,77,78,79,80,81,82,185,251,252,249,250,255,
  92,247,83,84,85,86,87,88,89,90,178,212,214,210,211,213,48,49,50,51,52,53,54,55,56,57,179,219,220,217,218,159};
bool segyTextIsEbcdic(const QByteArray &text) {
  int ascii = 0, ebcdic = 0;
  for (unsigned char c : text) {
    if (c == 0x20 || (c >= 0x30 && c <= 0x39)) ++ascii;          // ASCII 空格/数字
    else if (c == 0x40 || c >= 0xC1) ++ebcdic;                    // EBCDIC 空格/字母/数字
  }
  return ebcdic > ascii;
}
QByteArray encodeEbcdic(const QByteArray &latin1) {
  static unsigned char reverse[256]; static bool built = false;
  if (!built) { for (int i = 0; i < 256; ++i) reverse[kEbcdicToAscii[i]] = uchar(i); built = true; }
  QByteArray out(latin1.size(), '\0');
  for (int i = 0; i < latin1.size(); ++i) out[i] = char(reverse[uchar(latin1[i])]);
  return out;
}
bool isEndText(const QString &record) { return record.toUpper().contains(QStringLiteral("((SEG: ENDTEXT))")); }
}
namespace paleo::io {
QString segyTextHeaderToString(const QByteArray &text) {
  if (!segyTextIsEbcdic(text)) return QString::fromLatin1(text);
  QByteArray out(text.size(), '\0');
  for (int i = 0; i < text.size(); ++i) out[i] = char(kEbcdicToAscii[uchar(text[i])]);
  return QString::fromLatin1(out);
}
bool isMetricProjectCrs(const QString &crs) {
  OGRSpatialReference srs;
  return srs.SetFromUserInput(crs.toUtf8().constData(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) == OGRERR_NONE && srs.IsProjected()
      && std::abs(srs.GetLinearUnits() - 1.) < 1e-9;
}
QString coordinateWkt(const QString &crs) {
  OGRSpatialReference srs; char *wkt = nullptr;
  if (srs.SetFromUserInput(crs.toUtf8().constData(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE || srs.exportToWkt(&wkt) != OGRERR_NONE) return {};
  const auto result = QString::fromUtf8(wkt); CPLFree(wkt); return result;
}
bool projectRasterFile(const QString &path, const QString &target, QString *error, const QString &source) {
  GDALAllRegister();
  if (!source.isEmpty()) {
    // #859：此处以 GA_Update 原地写来源 CRS 声明——path 必须是私有暂存副本
    // （调用方 dataimport_geodata 传 staged），绝不能传用户的外部源文件。
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
      to.SetFromUserInput(target.toUtf8().constData(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE) {
    GDALClose(input); return fail(error, QStringLiteral("栅格来源或工程坐标系无效"));
  }
  from.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER); to.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
  if (from.IsSame(&to)) { GDALClose(input); return true; }
  // GDAL 3.13 按扩展名选驱动，裸 .partial 配不到 GTiff 会返回空。
  // .partial.tif 含 .partial，提交扫描会跳过；失败分支删掉它，不进工程。
  const auto destination = path + QStringLiteral(".partial.tif");
  QByteArray encoded = target.toUtf8();
  char *args[] = {const_cast<char *>("-t_srs"), encoded.data(),
                  const_cast<char *>("-of"), const_cast<char *>("GTiff"),
                  const_cast<char *>("-r"), const_cast<char *>("bilinear"), nullptr};
  auto *options = GDALWarpAppOptionsNew(args, nullptr);
  int usage = 0;
  const auto output = GDALWarp(destination.toUtf8().constData(), nullptr, 1,
                               &input, options, &usage);
  GDALWarpAppOptionsFree(options); GDALClose(input);
  if (!output) { QFile::remove(destination); return fail(error, QStringLiteral("栅格转换到工程基准坐标系失败")); }
  const auto targetWkt = coordinateWkt(target);
  if (targetWkt.isEmpty() ||
      GDALSetMetadataItem(output, "PALEO_CRS_WKT", targetWkt.toUtf8().constData(), nullptr) != CE_None) {
    GDALClose(output);
    QFile::remove(destination);
    return fail(error, QStringLiteral("无法保存工程坐标系声明"));
  }
  GDALClose(output);
  // Both files are private staged outputs; never replace external source files.
  // issue-810：先 rename 到 .old（腾位）再发布——先删后 rename 一旦失败，
  // 原文件已失、新文件也没上线，私有暂存栅格两头空。腾位失败即现场不动；
  // 发布失败把原文件改回原位（constraintstore 还原路径同款口径）。
  const auto backup = path + QStringLiteral(".crs-old");
  QFile::remove(backup);
  if (!QFile::rename(path, backup))
    return fail(error, QStringLiteral("无法保存坐标转换栅格"));
  if (!paleoReplaceFile(destination, path)) {
    QFile::rename(backup, path); // 回滚：原文件还给我
    QFile::remove(destination);
    return fail(error, QStringLiteral("无法保存坐标转换栅格"));
  }
  QFile::remove(backup);
  return true;
}
bool projectSegyFile(const QString &source, const QString &output, const QString &from,
                     const QString &target, QString *error) {
  auto ct = transformation(from, target);
  if (!ct) return fail(error, QStringLiteral("无法建立地震坐标转换"));
  OGRSpatialReference sourceReference;
  sourceReference.SetFromUserInput(from.toUtf8().constData(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get());
  QFile input(source);
  if (!input.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("无法读取地震源文件"));
  const auto header = input.read(3600);
  if (header.size() != 3600) return fail(error, QStringLiteral("地震头不完整"));
  // #855：与 SegyReader 同口径判字节序与扩展文本头。
  const bool le = SegyReader::detectLittleEndian(reinterpret_cast<const unsigned char *>(header.constData() + 3200));
  const int format = rd16(header.constData() + 3224, le);
  const int samples = rdU16(header.constData() + 3220, le);
  const int bytes = SegyReader::sampleBytesForFormat(format);
  const int extHeaders = rd16(header.constData() + 3504, le);
  if (!bytes || !samples || extHeaders < -1)
    return fail(error, QStringLiteral("当前地震格式不能安全转换坐标，请提供固定道长 SEG-Y"));
  qint64 firstTrace = 3600;
  if (extHeaders >= 0) firstTrace += qint64(extHeaders) * 3200;
  else {
    bool found = false;
    while (!found) {
      const auto record = input.read(3200);
      if (record.size() != 3200) return fail(error, QStringLiteral("扩展文本头缺少 EndText 记录"));
      firstTrace += 3200; found = isEndText(segyTextHeaderToString(record));
    }
  }
  const qint64 stride = 240 + qint64(samples) * bytes;
  if (input.size() < firstTrace || (input.size() - firstTrace) % stride)
    return fail(error, QStringLiteral("地震道长不一致，已拒绝坐标转换"));
  OGRSpatialReference targetReference;
  targetReference.SetFromUserInput(target.toUtf8().constData(), OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get());
  const bool targetGeographic = targetReference.IsGeographic();
  const QString partial = output + QStringLiteral(".partial");
  QFile::remove(partial);
  if (!QFile::copy(source, partial)) return fail(error, QStringLiteral("无法创建地震坐标转换副本"));
  QFile::setPermissions(partial, QFile::permissions(partial) | QFileDevice::WriteOwner);
  QFile copy(partial);
  const auto abortPartial = [&](const QString &message) {
    copy.close();
    QFile::remove(partial);
    return fail(error, message);
  };
  if (!copy.open(QIODevice::ReadWrite)) return abortPartial(QStringLiteral("无法写入地震副本"));
  for (qint64 offset = firstTrace; offset < copy.size(); offset += stride) {
    if (!copy.seek(offset)) return abortPartial(QStringLiteral("无法定位地震道头"));
    auto trace = copy.read(240);
    const int ns = trace.size() == 240 ? rdU16(trace.constData() + 114, le) : -1;
    if ((ns && ns != samples) || !transformHeader(trace, ct.get(), sourceReference.IsGeographic(), targetGeographic, le) || !copy.seek(offset) || copy.write(trace) != 240)
      return abortPartial(QStringLiteral("地震坐标转换失败；未发布半成品"));
  }
  // #856：按卷头原编码（EBCDIC/ASCII）写回；固定改写 C38（C39/C40 是 rev1 版本与
  // END TEXTUAL HEADER 标识，不动），另把原先声明 EPSG 的行改成新 CRS。
  const bool ebcdic = segyTextIsEbcdic(header.left(3200));
  const QString decoded = segyTextHeaderToString(header.left(3200));
  for (int line = 0; line < 38; ++line) {
    if (line != 37 && !decoded.mid(line * 80, 80).toUpper().contains(QStringLiteral("EPSG"))) continue;
    auto text = QStringLiteral("C%1 CRS %2; COORDINATES TRANSFORMED; SAMPLES UNCHANGED")
        .arg(line + 1, 2, 10, QLatin1Char('0')).arg(target).toLatin1().left(80).leftJustified(80, ' ');
    if (ebcdic) text = encodeEbcdic(text);
    if (!copy.seek(line * 80) || copy.write(text) != 80)
      return abortPartial(QStringLiteral("无法保存地震坐标声明"));
  }
  copy.close();
  // #859：替换语义发布（不先删目标）。
  if (!paleoReplaceFile(partial, output)) return abortPartial(QStringLiteral("无法发布地震坐标转换结果"));
  return true;
}
}
