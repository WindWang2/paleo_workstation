// 层：数据
#include "rasterpyramid.h"
#include "gdalreg_internal.h"

#include "pathcanon.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cpl_conv.h>
#include <gdal.h>
#include <gdal_priv.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace
{

using paleo::io_detail::ensureGdalRegistered;

  constexpr int kTileSize = 256;
  const char kLevelMagic[4] = {'P', 'Y', 'R', 'L'};
  constexpr qint64 kDefaultTileBudget = 128LL * 1024 * 1024;

  qint64 headerSizeFor(int tilesX, int tilesY)
  {
    return 28 + static_cast<qint64>(tilesX) * tilesY +      // state bytes
           static_cast<qint64>(tilesX) * tilesY * 8;        // offset table
  }

  // #215：瓦片 payload 偏移合法 = 落在 payload 追加区（头 + state + offset 表
  // 之后）且整块 samples*4 字节不越过文件尾。offset=0（崩溃残片）必判非法。
  bool payloadOffsetInRange(qint64 payloadOff, int tileCount, int samples, qint64 fileSize)
  {
    const qint64 payloadStart = 28 + static_cast<qint64>(tileCount) * 9;
    return payloadOff >= payloadStart &&
           payloadOff <= fileSize - static_cast<qint64>(samples) * 4;
  }

  bool payloadOffsetValid(QFile &f, qint64 offsetPos, int tileCount, int samples)
  {
    char off[8];
    if (!f.seek(offsetPos) || f.read(off, 8) != 8)
      return false;
    const qint64 payloadOff =
        static_cast<qint64>(qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(off)));
    return payloadOffsetInRange(payloadOff, tileCount, samples, f.size());
  }
} // namespace

RasterPyramidService::RasterPyramidService(QString cacheRoot)
    : m_root(std::move(cacheRoot)),
      m_tiles(QStringLiteral("pyramid-tiles"), kDefaultTileBudget,
              [](const std::shared_ptr<QVector<float>> &v) {
                return v ? static_cast<qint64>(v->size()) * 4 + 64 : 0;
              })
{
}

QString RasterPyramidService::keyFor(const QString &rasterPath) const
{
  const QByteArray h = QCryptographicHash::hash(
      PathCanon::canonicalize(rasterPath).toUtf8(), QCryptographicHash::Sha256);
  return QString::fromLatin1(h.toHex().left(24));
}

QString RasterPyramidService::dirFor(const QString &rasterPath) const
{
  return m_root + QLatin1Char('/') + keyFor(rasterPath);
}

void RasterPyramidService::levelGrid(const PyramidMeta &meta, int z, int *lw, int *lh,
                                     int *tilesX, int *tilesY)
{
  const int shift = 1 << z;
  *lw = qMax(1, (meta.width + shift - 1) / shift);
  *lh = qMax(1, (meta.height + shift - 1) / shift);
  *tilesX = (*lw + kTileSize - 1) / kTileSize;
  *tilesY = (*lh + kTileSize - 1) / kTileSize;
}

RasterPyramidService::SourceState *RasterPyramidService::stateFor(const QString &rasterPath)
{
  QMutexLocker lock(&m_stateMutex);
  const QString key = keyFor(rasterPath);
  auto it = m_states.find(key);
  if (it == m_states.end())
    it = m_states.insert(key, std::make_shared<SourceState>());
  return it.value().get();
}

bool RasterPyramidService::loadMeta(const QString &rasterPath, PyramidMeta *out) const
{
  QFile f(dirFor(rasterPath) + QStringLiteral("/meta.json"));
  if (!f.open(QIODevice::ReadOnly))
    return false;
  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
  if (!doc.isObject())
    return false;
  const QJsonObject o = doc.object();
  PyramidMeta m;
  m.sourcePath = o.value(QStringLiteral("source")).toString();
  m.sourceMtimeMs = static_cast<qint64>(o.value(QStringLiteral("mtime")).toDouble());
  m.sourceSize = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble());
  m.width = o.value(QStringLiteral("width")).toInt();
  m.height = o.value(QStringLiteral("height")).toInt();
  m.bandCount = o.value(QStringLiteral("bands")).toInt(1);
  m.hasNodata = o.value(QStringLiteral("has_nodata")).toBool(false);
  m.nodata = static_cast<float>(o.value(QStringLiteral("nodata")).toDouble(-9999.0));
  m.levels = o.value(QStringLiteral("levels")).toInt();
  m.crsWkt = o.value(QStringLiteral("crs")).toString();
  const QJsonArray gt = o.value(QStringLiteral("gt")).toArray();
  for (int i = 0; i < 6 && i < gt.size(); ++i)
    m.gt[i] = gt.at(i).toDouble();
  if (m.width <= 0 || m.height <= 0 || m.levels <= 0 || m.sourcePath.isEmpty())
    return false;
  *out = m;
  return true;
}

bool RasterPyramidService::writeMeta(const QString &rasterPath, const PyramidMeta &meta) const
{
  const QString dir = dirFor(rasterPath);
  if (!QDir().mkpath(dir))
    return false;
  QJsonObject o;
  o.insert(QStringLiteral("source"), meta.sourcePath);
  o.insert(QStringLiteral("mtime"), double(meta.sourceMtimeMs));
  o.insert(QStringLiteral("size"), double(meta.sourceSize));
  o.insert(QStringLiteral("width"), meta.width);
  o.insert(QStringLiteral("height"), meta.height);
  o.insert(QStringLiteral("bands"), meta.bandCount);
  o.insert(QStringLiteral("has_nodata"), meta.hasNodata);
  o.insert(QStringLiteral("nodata"), double(meta.nodata));
  o.insert(QStringLiteral("levels"), meta.levels);
  o.insert(QStringLiteral("crs"), meta.crsWkt);
  QJsonArray gt;
  for (int i = 0; i < 6; ++i)
    gt.append(meta.gt[i]);
  o.insert(QStringLiteral("gt"), gt);
  QSaveFile f(dir + QStringLiteral("/meta.json"));
  f.setDirectWriteFallback(false);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  const QByteArray bytes = QJsonDocument(o).toJson(QJsonDocument::Compact);
  return f.write(bytes) == bytes.size() && f.commit();
}

bool RasterPyramidService::ensureLevelFile(const QString &rasterPath,
                                           const PyramidMeta &meta, int z) const
{
  const QString path = dirFor(rasterPath) + QStringLiteral("/z%1.bin").arg(z);
  if (QFile::exists(path))
    return true;
  int lw = 0, lh = 0, tilesX = 0, tilesY = 0;
  levelGrid(meta, z, &lw, &lh, &tilesX, &tilesY);
  QByteArray header;
  header.append(kLevelMagic, 4);
  cacheio::putU32(&header, static_cast<quint32>(z));
  cacheio::putU32(&header, static_cast<quint32>(tilesX));
  cacheio::putU32(&header, static_cast<quint32>(tilesY));
  cacheio::putU32(&header, static_cast<quint32>(qMin(kTileSize, lw)));
  cacheio::putU32(&header, static_cast<quint32>(qMin(kTileSize, lh)));
  const int n = tilesX * tilesY;
  header.append(QByteArray(n, '\0'));       // state 全 0（未建）
  header.append(QByteArray(n * 8, '\0'));   // offset table 全 0
  QSaveFile f(path);
  f.setDirectWriteFallback(false);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  if (f.write(header) != header.size() || !f.commit())
    return false;
  return true;
}

bool RasterPyramidService::ensure(const QString &rasterPath, PyramidMeta *out,
                                  QString *error, BuildStrategy strategy)
{
  const QFileInfo info(rasterPath);
  if (!info.exists())
  {
    if (error) *error = QStringLiteral("no such raster: %1").arg(rasterPath);
    return false;
  }
  const QString canon = PathCanon::canonicalize(rasterPath);
  const qint64 mtime = info.lastModified().toMSecsSinceEpoch();

  PyramidMeta meta;
  if (loadMeta(rasterPath, &meta) && meta.sourcePath == canon &&
      meta.sourceMtimeMs == mtime && meta.sourceSize == info.size())
  {
    // 命中：目录与层级文件在即可（防半建目录缺层）。
    for (int z = 0; z < meta.levels; ++z)
      if (!ensureLevelFile(rasterPath, meta, z))
      {
        if (error) *error = QStringLiteral("cannot init level file z%1").arg(z);
        return false;
      }
    if (strategy == BuildStrategy::Lazy)
    {
      if (out) *out = meta;
      return true;
    }
    // Eager：全部瓦片（已在的跳过——buildTile 落盘前查 state）。
    SourceState *st = stateFor(rasterPath);
    QMutexLocker build(&st->buildMutex);
    for (int z = 0; z < meta.levels; ++z)
    {
      int lw = 0, lh = 0, tilesX = 0, tilesY = 0;
      levelGrid(meta, z, &lw, &lh, &tilesX, &tilesY);
      for (int y = 0; y < tilesY; ++y)
        for (int x = 0; x < tilesX; ++x)
        {
          Tile t;
          if (!buildTile(rasterPath, meta, z, x, y, &t, error))
            return false;
        }
    }
    if (out) *out = meta;
    return true;
  }

  // 新建（或源变了重建）。
  invalidate(rasterPath);
  ensureGdalRegistered();
  GDALDatasetH ds = GDALOpen(canon.toUtf8().constData(), GA_ReadOnly);
  if (!ds)
  {
    if (error) *error = QStringLiteral("cannot open raster %1").arg(rasterPath);
    return false;
  }
  meta = PyramidMeta();
  meta.sourcePath = canon;
  meta.sourceMtimeMs = mtime;
  meta.sourceSize = info.size();
  meta.width = GDALGetRasterXSize(ds);
  meta.height = GDALGetRasterYSize(ds);
  meta.bandCount = GDALGetRasterCount(ds);
  GDALRasterBandH band = GDALGetRasterBand(ds, 1);
  int hasNd = 0;
  const double nd = GDALGetRasterNoDataValue(band, &hasNd);
  meta.hasNodata = hasNd != 0;
  if (hasNd)
    meta.nodata = static_cast<float>(nd);
  double gt[6] = {0, 1, 0, 0, 0, 1};
  GDALGetGeoTransform(ds, gt);
  for (int i = 0; i < 6; ++i)
    meta.gt[i] = gt[i];
  const char *proj = GDALGetProjectionRef(ds);
  if (proj)
    meta.crsWkt = QString::fromLatin1(proj);
  GDALClose(ds);
  if (meta.width <= 0 || meta.height <= 0)
  {
    if (error) *error = QStringLiteral("empty raster %1").arg(rasterPath);
    return false;
  }
  // 层级数：最高层缩到 ≤256×256 为止。
  int maxZ = 0;
  while (true)
  {
    const int shift = 1 << maxZ;
    if (qMax((meta.width + shift - 1) / shift, (meta.height + shift - 1) / shift) <= kTileSize)
      break;
    ++maxZ;
  }
  meta.levels = maxZ + 1;
  if (!writeMeta(rasterPath, meta))
  {
    if (error) *error = QStringLiteral("cannot write pyramid meta for %1").arg(rasterPath);
    return false;
  }
  for (int z = 0; z < meta.levels; ++z)
    if (!ensureLevelFile(rasterPath, meta, z))
    {
      if (error) *error = QStringLiteral("cannot init level file z%1").arg(z);
      return false;
    }
  if (strategy == BuildStrategy::Eager)
    return ensure(rasterPath, out, error, BuildStrategy::Eager); // 走命中分支的 Eager 路径
  if (out) *out = meta;
  return true;
}

bool RasterPyramidService::isBuilt(const QString &rasterPath) const
{
  PyramidMeta meta;
  return loadMeta(rasterPath, &meta);
}

bool RasterPyramidService::buildTile(const QString &rasterPath, const PyramidMeta &meta,
                                     int z, int x, int y, Tile *out, QString *error) const
{
  int lw = 0, lh = 0, tilesX = 0, tilesY = 0;
  levelGrid(meta, z, &lw, &lh, &tilesX, &tilesY);
  if (x < 0 || y < 0 || x >= tilesX || y >= tilesY)
  {
    if (error) *error = QStringLiteral("tile %1/%2/%3 out of grid").arg(z).arg(x).arg(y);
    return false;
  }
  const int tileW = qMin(kTileSize, lw - x * kTileSize);
  const int tileH = qMin(kTileSize, lh - y * kTileSize);
  const int shift = 1 << z;
  const int srcX = x * kTileSize * shift;
  const int srcY = y * kTileSize * shift;
  const int srcW = qMin(tileW * shift, meta.width - srcX);
  const int srcH = qMin(tileH * shift, meta.height - srcY);
  if (srcW <= 0 || srcH <= 0)
  {
    // 完全在栅格外：按全 nodata 处理。
    out->z = z; out->x = x; out->y = y;
    out->width = tileW; out->height = tileH;
    out->allNodata = true;
    out->samples.clear();
    // 记 state=2（若尚为 0）。
    const QString path = dirFor(rasterPath) + QStringLiteral("/z%1.bin").arg(z);
    QFile f(path);
    if (f.open(QIODevice::ReadWrite))
    {
      const qint64 statePos = 28 + static_cast<qint64>(y) * tilesX + x;
        if (f.seek(statePos) && f.peek(1).at(0) == '\0')
        {
          f.seek(statePos);
          f.putChar('\2');
        }
    }
    return true;
  }

  ensureGdalRegistered();
  GDALDatasetH ds = GDALOpen(meta.sourcePath.toUtf8().constData(), GA_ReadOnly);
  if (!ds)
  {
    if (error) *error = QStringLiteral("cannot reopen %1").arg(meta.sourcePath);
    return false;
  }
  QVector<float> buf(tileW * tileH, 0.0f);
  GDALRasterIOExtraArg extra;
  INIT_RASTERIO_EXTRA_ARG(extra);
  extra.eResampleAlg = GRIORA_Average;
  const CPLErr rc = GDALRasterIOEx(GDALGetRasterBand(ds, 1), GF_Read, srcX, srcY, srcW,
                                   srcH, buf.data(), tileW, tileH, GDT_Float32, 0, 0,
                                   &extra);
  GDALClose(ds);
  if (rc != CE_None)
  {
    if (error) *error = QStringLiteral("raster io failed for tile %1/%2/%3").arg(z).arg(x).arg(y);
    return false;
  }
  bool allNodata = true;
  for (float v : buf)
  {
    if (!meta.hasNodata || v != meta.nodata)
    {
      allNodata = false;
      break;
    }
  }
  out->z = z; out->x = x; out->y = y;
  out->width = tileW; out->height = tileH;
  out->allNodata = allNodata;
  out->samples = allNodata ? QVector<float>() : buf;

  // 持久化：state + 追加 payload + offset（全 nodata 只写 state=2）。
  const QString path = dirFor(rasterPath) + QStringLiteral("/z%1.bin").arg(z);
  QFile f(path);
  if (!f.open(QIODevice::ReadWrite))
  {
    if (error) *error = QStringLiteral("cannot open level pack %1").arg(path);
    return false;
  }
  const int n = tilesX * tilesY;
  const qint64 statePos = 28 + static_cast<qint64>(y) * tilesX + x;
  const qint64 offsetPos = 28 + n + (static_cast<qint64>(y) * tilesX + x) * 8;
  if (f.seek(statePos))
  {
    const char st = f.peek(1).at(0);
    // 已建过——不重复落盘（并发/重复 ensure 兜底）。#215：state=1 但 offset
    // 不指向合法 payload 的是崩溃/写失败残片，必须重建覆盖而非早退。
    if (st == '\2' || (st == '\1' && payloadOffsetValid(f, offsetPos, n, tileW * tileH)))
      return true;
  }
  // #215：按头文件协议「payload append → 回填 offset → 最后写 state」，
  // 任一步失败 state 保持 0（未建），下次 tile() 自然重建。
  if (!allNodata)
  {
    QByteArray payload;
    payload.reserve(tileW * tileH * 4);
    for (float v : buf)
      cacheio::putF32(&payload, v);
    const qint64 end = f.size();
    if (!f.seek(end) || f.write(payload) != payload.size())
    {
      if (error) *error = QStringLiteral("short write to %1").arg(path);
      return false;
    }
    char off[8];
    qToLittleEndian<quint64>(static_cast<quint64>(end), off);
    if (!f.seek(offsetPos) || f.write(off, 8) != 8 || !f.flush())
    {
      if (error) *error = QStringLiteral("offset write failed in %1").arg(path);
      return false;
    }
  }
  if (!f.seek(statePos) || !f.putChar(allNodata ? '\2' : '\1') || !f.flush())
  {
    if (error) *error = QStringLiteral("state write failed in %1").arg(path);
    return false;
  }
  return true;
}

bool RasterPyramidService::tile(const QString &rasterPath, int z, int x, int y, Tile *out,
                                QString *error)
{
  PyramidMeta meta;
  if (!loadMeta(rasterPath, &meta))
  {
    // 隐式懒建目录（D3.3）。
    if (!ensure(rasterPath, &meta, error, BuildStrategy::Lazy))
      return false;
  }
  if (z < 0 || z >= meta.levels)
  {
    if (error) *error = QStringLiteral("level %1 out of range (0..%2)").arg(z).arg(meta.levels - 1);
    return false;
  }
  int lw = 0, lh = 0, tilesX = 0, tilesY = 0;
  levelGrid(meta, z, &lw, &lh, &tilesX, &tilesY);
  if (x < 0 || y < 0 || x >= tilesX || y >= tilesY)
  {
    if (error) *error = QStringLiteral("tile %1/%2/%3 out of grid %4x%5").arg(z).arg(x).arg(y).arg(tilesX).arg(tilesY);
    return false;
  }
  const QString key = QStringLiteral("%1|%2/%3/%4").arg(keyFor(rasterPath)).arg(z).arg(x).arg(y);

  // LRU 命中（D3.4）。
  if (const auto hit = m_tiles.get(key))
  {
    out->z = z; out->x = x; out->y = y;
    out->width = qMin(kTileSize, lw - x * kTileSize);
    out->height = qMin(kTileSize, lh - y * kTileSize);
    out->samples = **hit;
    out->allNodata = false;
    return true;
  }

  // 磁盘层：state + offset 直读。
  const QString path = dirFor(rasterPath) + QStringLiteral("/z%1.bin").arg(z);
  QFile f(path);
  if (f.open(QIODevice::ReadOnly))
  {
    const int n = tilesX * tilesY;
    const qint64 statePos = 28 + static_cast<qint64>(y) * tilesX + x;
    const qint64 offsetPos = 28 + n + (static_cast<qint64>(y) * tilesX + x) * 8;
    if (f.seek(statePos))
    {
      const char st = f.peek(1).at(0);
      if (st == '\2')
      {
        out->z = z; out->x = x; out->y = y;
        out->width = qMin(kTileSize, lw - x * kTileSize);
        out->height = qMin(kTileSize, lh - y * kTileSize);
        out->allNodata = true;
        out->samples.clear();
        return true;
      }
      if (st == '\1' && f.seek(offsetPos))
      {
        char off[8];
        if (f.read(off, 8) == 8)
        {
          const qint64 payloadOff =
              static_cast<qint64>(qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(off)));
          const int w = qMin(kTileSize, lw - x * kTileSize);
          const int h = qMin(kTileSize, lh - y * kTileSize);
          QByteArray raw(w * h * 4, Qt::Uninitialized);
          // #215：offset 必须落在 payload 追加区内（>= 头+state+offset 表），
          // 否则（如崩溃残片 offset=0）落到下方懒建分支重建，不把文件头当像素。
          if (payloadOffsetInRange(payloadOff, n, w * h, f.size()) && f.seek(payloadOff) &&
              f.read(raw.data(), raw.size()) == raw.size())
          {
            auto samples = std::make_shared<QVector<float>>();
            samples->resize(w * h);
            qint64 pos = 0;
            bool ok = true;
            for (int i = 0; i < w * h && ok; ++i)
              (*samples)[i] = cacheio::f32(raw, &pos, &ok);
            if (ok)
            {
              m_tiles.insert(key, samples);
              out->z = z; out->x = x; out->y = y;
              out->width = w; out->height = h;
              out->allNodata = false;
              out->samples = *samples;
              return true;
            }
          }
        }
      }
    }
  }

  // 未建：懒生成（同源互斥，D3.8）。
  SourceState *st = stateFor(rasterPath);
  QMutexLocker build(&st->buildMutex);
  // 双检（并发首建可能已落盘）。
  if (const auto hit = m_tiles.get(key))
  {
    out->z = z; out->x = x; out->y = y;
    out->width = qMin(kTileSize, lw - x * kTileSize);
    out->height = qMin(kTileSize, lh - y * kTileSize);
    out->samples = **hit;
    out->allNodata = false;
    return true;
  }
  if (!buildTile(rasterPath, meta, z, x, y, out, error))
    return false;
  if (!out->allNodata)
    m_tiles.insert(key, std::make_shared<QVector<float>>(out->samples));
  return true;
}

RasterPyramidService::Stats RasterPyramidService::stats(const QString &rasterPath) const
{
  Stats st;
  PyramidMeta meta;
  if (!loadMeta(rasterPath, &meta))
    return st;
  st.levels = meta.levels;
  for (int z = 0; z < meta.levels; ++z)
  {
    const QString path = dirFor(rasterPath) + QStringLiteral("/z%1.bin").arg(z);
    QFileInfo info(path);
    st.bytesOnDisk += info.size();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      continue;
    int lw = 0, lh = 0, tilesX = 0, tilesY = 0;
    levelGrid(meta, z, &lw, &lh, &tilesX, &tilesY);
    const int n = tilesX * tilesY;
    if (f.size() < 28 + n + n * 8)
      continue;
    f.seek(28);
    const QByteArray states = f.read(n);
    for (char s : states)
    {
      if (s == '\1') ++st.tilesStored;
      else if (s == '\2') ++st.tilesNodata;
      else ++st.tilesUnbuilt;
    }
  }
  return st;
}

void RasterPyramidService::invalidate(const QString &rasterPath)
{
  const QString dir = dirFor(rasterPath);
  QDir(dir).removeRecursively();
  const QString prefix = keyFor(rasterPath) + QLatin1Char('|');
  const QVector<QString> keys = m_tiles.keys();
  for (const QString &k : keys)
    if (k.startsWith(prefix))
      m_tiles.erase(k);
}

void RasterPyramidService::setTileCacheBudget(qint64 bytes)
{
  m_tiles.setCapacity(bytes);
}

CacheStats RasterPyramidService::cacheStats() const
{
  return m_tiles.stats();
}

void RasterPyramidService::pinTile(const QString &rasterPath, int z, int x, int y)
{
  m_tiles.setPin(QStringLiteral("%1|%2/%3/%4").arg(keyFor(rasterPath)).arg(z).arg(x).arg(y), true);
}

void RasterPyramidService::unpinTile(const QString &rasterPath, int z, int x, int y)
{
  m_tiles.setPin(QStringLiteral("%1|%2/%3/%4").arg(keyFor(rasterPath)).arg(z).arg(x).arg(y), false);
}
