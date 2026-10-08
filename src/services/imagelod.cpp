// 层：数据
#include "imagelod.h"

#include <QBrush>
#include <QImageReader>

namespace paleo::imagelod
{

TrackImage loadThumbnail(const QString &path, int edge)
{
  TrackImage out;
  if (edge <= 0)
    edge = LodPolicy::kThumbnailEdge;
  // QImageReader 不可拷贝/移动——就地构造（EXIF Orientation 自动应用，
  // 装载路径唯一，道内正立）。
  QImageReader reader(path);
  reader.setAutoTransform(true);
  const QSize raw = reader.size();
  if (raw.isValid())
  {
    out.fullSize = raw;
    // 只缩不放：小于缩略边的图保持原尺寸（放大无质量收益，徒占内存）。
    if (raw.width() > edge || raw.height() > edge)
    {
      const double scale = qMin(double(edge) / raw.width(),
                                double(edge) / raw.height());
      reader.setScaledSize(QSize(qMax(1, qRound(raw.width() * scale)),
                                 qMax(1, qRound(raw.height() * scale))));
    }
  }
  out.thumbnail = reader.read();
  if (out.thumbnail.isNull())
    return out;
  out.hasAlpha = out.thumbnail.hasAlphaChannel();
  if (!out.fullSize.isValid())
    out.fullSize = out.thumbnail.size();
  return out;
}

QImage loadFull(const QString &path)
{
  QImageReader reader(path);
  reader.setAutoTransform(true);
  return reader.read();
}

QImage FullImageCache::peek(const QString &key) const
{
  return m_map.value(key);
}

QImage FullImageCache::acquire(const QString &key, const QString &path)
{
  const auto it = m_map.constFind(key);
  if (it != m_map.constEnd())
  {
    const qsizetype idx = m_order.indexOf(key);
    if (idx >= 0)
    {
      m_order.removeAt(idx);
      m_order.append(key);
    }
    return it.value();
  }
  const QImage img = loadFull(path);
  if (img.isNull())
    return img; // 装载失败不占额（下次触发重试——文件可能已恢复）
  m_order.append(key);
  m_map.insert(key, img);
  while (m_order.size() > LodPolicy::kFullCacheEntries)
  {
    m_map.remove(m_order.front());
    m_order.removeFirst();
  }
  return img;
}

void FullImageCache::clear()
{
  m_order.clear();
  m_map.clear();
}

FullImageCache &FullImageCache::shared()
{
  static FullImageCache instance;
  return instance;
}

QBrush alphaCheckerboard(int cellPx)
{
  if (cellPx <= 0)
    cellPx = 6;
  QImage tile(cellPx * 2, cellPx * 2, QImage::Format_RGB32);
  const QRgb light = qRgb(0xEE, 0xEE, 0xEE);
  const QRgb dark = qRgb(0xBB, 0xBB, 0xBB);
  for (int y = 0; y < tile.height(); ++y)
    for (int x = 0; x < tile.width(); ++x)
      tile.setPixel(x, y,
                    (x / cellPx + y / cellPx) % 2 == 0 ? light : dark);
  return QBrush(tile);
}

} // namespace paleo::imagelod
