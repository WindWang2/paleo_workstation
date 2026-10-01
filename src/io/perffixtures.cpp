// 层：数据
#include "perffixtures.h"

#include "../catalog/datacatalog.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <cpl_conv.h>
#include <gdal.h>
#include <gdal_priv.h>

#include <cmath>
#include <cstring>
#include <mutex>

namespace
{
  void ensureGdalRegistered()
  {
    static std::once_flag flag;
    std::call_once(flag, []() { GDALAllRegister(); });
  }

  quint32 lcg(quint32 &state)
  {
    state = state * 1664525u + 1013904223u;
    return state;
  }
} // namespace

namespace PerfFixtures
{

quint32 lcgNext(quint32 &state)
{
  return lcg(state);
}

double lcgUnit(quint32 &state)
{
  return double(lcg(state) >> 8) / double(1u << 24);
}

bool makeSyntheticLas(const QString &path, int rows, const QStringList &curves,
                      double startDepth, double stepDepth, QString *error)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (error)
      *error = QStringLiteral("cannot write %1").arg(path);
    return false;
  }
  QByteArray out;
  out.reserve(rows * curves.size() * 12 + 1024);
  out += "~VERSION INFORMATION\n";
  out += "VERS. 2.0 : CWLS log ASCII Standard\n";
  out += "WRAP. NO  : one line per depth step\n";
  out += "~WELL INFORMATION\n";
  out += "STRT.M 1000.000 : first depth\n";
  out += "STOP.M 2949.875 : last depth\n";
  out += "NULL. -999.25 : null value\n";
  out += "WELL. SYNTH-01 : synthetic benchmark well\n";
  out += "~CURVE INFORMATION\n";
  for (const QString &c : curves)
  {
    const QString unit = c == QLatin1String("DEPT") ? QStringLiteral("M")
                         : c == QLatin1String("DT") ? QStringLiteral("US/M")
                         : c == QLatin1String("GR") ? QStringLiteral("GAPI")
                                                    : QStringLiteral("G/CM3");
    out += QStringLiteral("%1.%2 : curve %1\n").arg(c, unit).toUtf8();
  }
  out += "~ASCII LOG DATA\n";
  quint32 rng = 20260929u;
  for (int r = 0; r < rows; ++r)
  {
    const double depth = startDepth + r * stepDepth;
    QString line;
    line += QString::number(depth, 'f', 3);
    for (int c = 1; c < curves.size(); ++c)
    {
      if (r % 97 == 13)
      {
        line += QStringLiteral(" -999.25"); // NULL 值埋点（NaN 映射断言）
        continue;
      }
      const double v = 20.0 + 100.0 * lcgUnit(rng) +
                       10.0 * std::sin(depth * 0.01 + double(c));
      line += QStringLiteral(" %1").arg(QString::number(v, 'f', 4));
    }
    out += line.toUtf8();
    out += '\n';
  }
  if (f.write(out) != out.size())
  {
    if (error)
      *error = QStringLiteral("short write to %1").arg(path);
    return false;
  }
  return true;
}

int makeSyntheticSegy(const QString &path, int inlCount, int xlCount, int samples,
                      qint32 baseInline, qint32 baseXline, int dtUs, QString *error)
{
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
  {
    if (error)
      *error = QStringLiteral("cannot write %1").arg(path);
    return -1;
  }
  // 文本头 3200B（ASCII 填充 + C01 标识行）。
  QByteArray text(3200, ' ');
  const QString mark = QStringLiteral("C01 SYNTHETIC BENCHMARK SURVEY");
    memcpy(text.data(), mark.toLatin1().constData(), qMin(mark.size(), 3200));
  f.write(text);
  // 二进制头 400B：sampleInterval(dt)、samples、format 5(IEEE)、extHeaders 0。
  QByteArray bin(400, '\0');
  auto putU16 = [&bin](int off, quint16 v) {
    char b[2];
    qToBigEndian<quint16>(v, b);
    memcpy(bin.data() + off, b, 2);
  };
  auto putU32 = [&bin](int off, quint32 v) {
    char b[4];
    qToBigEndian<quint32>(v, b);
    memcpy(bin.data() + off, b, 4);
  };
  putU16(16, static_cast<quint16>(dtUs));
  putU16(20, static_cast<quint16>(samples));
  putU16(24, 5); // IEEE
  putU16(304, 0); // 无扩展文本头
  putU16(12, static_cast<quint16>(xlCount)); // 每条 inline 道数（D61 口径）
  putU32(4, static_cast<quint32>(baseInline));
  f.write(bin);

  quint32 rng = 42u;
  int traceIndex = 0;
  QByteArray trHdr(240, '\0');
  QByteArray samples4(static_cast<int>(samples) * 4, '\0');
  for (int i = 0; i < inlCount; ++i)
  {
    for (int x = 0; x < xlCount; ++x, ++traceIndex)
    {
      memset(trHdr.data(), 0, 240);
      auto putHdrU16 = [&trHdr](int off, quint16 v) {
        char b[2];
        qToBigEndian<quint16>(v, b);
        memcpy(trHdr.data() + off, b, 2);
      };
      auto putHdrU32 = [&trHdr](int off, qint32 v) {
        char b[4];
        qToBigEndian<qint32>(v, b);
        memcpy(trHdr.data() + off, b, 4);
      };
      putHdrU32(0, traceIndex + 1);         // TRACL
      putHdrU32(8, i + 1);                  // field record
      putHdrU32(20, x);                     // CDP（= xline 序号）
      const double cx = 500000.0 + 50.0 * x;
      const double cy = 4000000.0 + 100.0 * i;
      putHdrU16(70, 1);                     // 坐标比例 = 1
      putHdrU32(72, static_cast<qint32>(cx));
      putHdrU32(76, static_cast<qint32>(cy));
      putHdrU16(108, 0);                    // delay
      putHdrU16(114, static_cast<quint16>(samples));
      putHdrU16(116, static_cast<quint16>(dtUs));
      putHdrU32(188, baseInline + i);       // inline
      putHdrU32(192, baseXline + x);        // xline
      f.write(trHdr);
      for (int s = 0; s < samples; ++s)
      {
        const float v = float(lcgUnit(rng)) - 0.5f;
        char b[4];
        memcpy(b, &v, 4);
        quint32 be = 0;
        memcpy(&be, b, 4);
        qToBigEndian<quint32>(be, samples4.data() + s * 4);
      }
      f.write(samples4);
    }
  }
  return traceIndex;
}

bool populateSyntheticCatalog(DataCatalog *catalog, int nAssets, QString *error)
{
  if (!catalog->isOpen())
  {
    if (error)
      *error = QStringLiteral("catalog is not open");
    return false;
  }
  const QString projectDir = QFileInfo(catalog->catalogPath()).absolutePath().section(
      QStringLiteral("/artifacts/metadata"), 0, 0);
  // WP2：分段计时（PALEO_CATALOG_PROFILE 非空即开）——mutator 超线性归因用；
  // 关闭时零开销（两次 env 查询 + 每资产 5 次 elapsed 判断）。
  const bool profile = !qEnvironmentVariableIsEmpty("PALEO_CATALOG_PROFILE");
  QElapsedTimer phase;
  qint64 tEntity = 0, tAsset = 0, tVersion = 0, tLink = 0, tFiles = 0;
  DataCatalog::BatchSave batch(catalog);
  for (int i = 0; i < nAssets; ++i)
  {
    CatalogEntity e;
    e.id = QStringLiteral("well-%1").arg(i + 1, 6, 10, QLatin1Char('0'));
    e.entityType = QStringLiteral("well");
    e.name = QStringLiteral("SYNTH-%1").arg(i + 1, 6, 10, QLatin1Char('0'));
    e.hasSurface = true;
    e.surfaceX = 500000.0 + i * 13.0;
    e.surfaceY = 4000000.0 + i * 7.0;
    if (profile) phase.start();
    if (!catalog->addEntity(e, error))
      return false;
    if (profile) tEntity += phase.nsecsElapsed();
    CatalogAsset a;
    a.id = QStringLiteral("ast-%1").arg(i + 1);
    a.type = QStringLiteral("well_log");
    a.format = QStringLiteral("las");
    a.displayName = QStringLiteral("synth_%1.las").arg(i + 1);
    if (profile) phase.start();
    if (!catalog->addAsset(a, error))
      return false;
    if (profile) tAsset += phase.nsecsElapsed();
    CatalogVersion v;
    v.id = QStringLiteral("ver-%1").arg(i + 1);
    v.assetId = a.id;
    v.stage = QStringLiteral("RAW");
    v.versionNumber = 1;
    v.managed = true;
    v.fileName = QStringLiteral("synth_%1.las").arg(i + 1);
    v.path = DataCatalog::managedPath(QStringLiteral("RAW"), a.id, v.id, v.fileName);
    if (profile) phase.start();
    if (v.path.isEmpty() || !catalog->addVersion(v, error))
      return false;
    if (profile) tVersion += phase.nsecsElapsed();
    // 受管文件落盘（小内容即可——catalog 查询基准不读它们）。
    if (profile) phase.start();
    const QString abs = projectDir + QLatin1Char('/') + v.path;
    QDir().mkpath(QFileInfo(abs).absolutePath());
    QFile file(abs);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      file.write(QStringLiteral("SYNTH fixture payload %1\n").arg(i).toUtf8());
    if (profile) tFiles += phase.nsecsElapsed();
    EntityAssetLink l;
    l.entityType = QStringLiteral("well");
    l.entityId = e.id;
    l.assetId = a.id;
    l.role = QStringLiteral("well_log");
    l.isPrimary = true;
    if (profile) phase.start();
    if (!catalog->addLink(l, error))
      return false;
    if (profile) tLink += phase.nsecsElapsed();
  }
  if (profile) phase.start();
  const bool ok = batch.flush(error);
  if (profile)
    qInfo("PERF populate phases(ms) n=%d entity=%.1f asset=%.1f version=%.1f "
          "files=%.1f link=%.1f flush=%.1f",
          nAssets, tEntity / 1.0e6, tAsset / 1.0e6, tVersion / 1.0e6,
          tFiles / 1.0e6, tLink / 1.0e6, phase.nsecsElapsed() / 1.0e6);
  return ok;
}

bool makeSyntheticCatalogDir(const QString &dir, int nAssets, QString *error)
{
  QDir().mkpath(dir + QStringLiteral("/artifacts/metadata"));
  DataCatalog catalog;
  if (!catalog.open(dir, error))
    return false;
  return populateSyntheticCatalog(&catalog, nAssets, error);
}

bool makeSyntheticWellFiles(const QString &dir, int wellCount, QString *error)
{
  if (!QDir().mkpath(dir))
  {
    if (error)
      *error = QStringLiteral("cannot create %1").arg(dir);
    return false;
  }
  QByteArray heads, tops, td;
  for (int i = 0; i < wellCount; ++i)
  {
    heads += QStringLiteral("SYNTH-%1 %2 %3 0.0 3000.0\n")
                 .arg(i + 1, 4, 10, QLatin1Char('0'))
                 .arg(500000.0 + i * 11.0, 0, 'f', 1)
                 .arg(4000000.0 + i * 5.0, 0, 'f', 1)
                 .toUtf8();
    tops += QStringLiteral("SYNTH-%1 SB1 2500.0\n")
                .arg(i + 1, 4, 10, QLatin1Char('0'))
                .toUtf8();
    td += QStringLiteral("# Well : SYNTH-%1\n0.0 0.0 0.0 0.0\n100.0 120.0 110.0 90.0\n")
              .arg(i + 1, 4, 10, QLatin1Char('0'))
              .toUtf8();
  }
  QFile f1(dir + QStringLiteral("/ExportWellHead.dat"));
  QFile f2(dir + QStringLiteral("/tops.dat"));
  QFile f3(dir + QStringLiteral("/td.dat"));
  const bool ok = f1.open(QIODevice::WriteOnly) && f2.open(QIODevice::WriteOnly) &&
                  f3.open(QIODevice::WriteOnly);
  if (!ok)
  {
    if (error)
      *error = QStringLiteral("cannot write well fixtures under %1").arg(dir);
    return false;
  }
  f1.write(heads);
  f2.write(tops);
  f3.write(td);
  return true;
}

bool makeSyntheticGeoTiff(const QString &path, int w, int h, bool withNodataHole,
                          QString *error)
{
  ensureGdalRegistered();
  GDALDriverH drv = GDALGetDriverByName("GTiff");
  if (!drv)
  {
    if (error)
      *error = QStringLiteral("GTiff driver unavailable");
    return false;
  }
  GDALDatasetH ds = GDALCreate(drv, path.toUtf8().constData(), w, h, 1, GDT_Float32,
                               nullptr);
  if (!ds)
  {
    if (error)
      *error = QStringLiteral("cannot create %1").arg(path);
    return false;
  }
  const double gt[6] = {500000.0, 25.0, 0.0, 4000000.0, 0.0, -25.0};
  GDALSetGeoTransform(ds, gt);
  GDALSetProjection(ds, "ENGCRS[\"Paleo local engineering grid\",EDATUM[\"Local engineering datum\"],CS[Cartesian,2],AXIS[\"easting\",east,ORDER[1],LENGTHUNIT[\"metre\",1]],AXIS[\"northing\",north,ORDER[2],LENGTHUNIT[\"metre\",1]]]");
  GDALRasterBandH band = GDALGetRasterBand(ds, 1);
  const float nodata = -9999.0f;
  GDALSetRasterNoDataValue(band, nodata);
  QVector<float> line(w);
  for (int y = 0; y < h; ++y)
  {
    for (int x = 0; x < w; ++x)
    {
      const double v = 1000.0 + 50.0 * std::sin(x * 0.05) + 30.0 * std::cos(y * 0.07);
      line[x] = static_cast<float>(v);
    }
    if (withNodataHole && y >= h / 3 && y < 2 * h / 3)
    {
      for (int x = w / 3; x < 2 * w / 3; ++x)
        line[x] = nodata;
    }
    if (GDALRasterIO(band, GF_Write, 0, y, w, 1, line.data(), w, 1, GDT_Float32, 0, 0) !=
        CE_None)
    {
      if (error)
        *error = QStringLiteral("write failed at line %1").arg(y);
      GDALClose(ds);
      return false;
    }
  }
  GDALClose(ds);
  return true;
}

} // namespace PerfFixtures
