// 层：数据
#include "imagelod.h"

#include <QBrush>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QImageReader>
#include <QObject>
#include <QSet>
#include <QThread>
#include <QtConcurrent>

namespace paleo::imagelod
{

namespace
{

std::function<void()> g_fullLoadHook;

// 解码在途表与代际。只在 GUI 线程读写（requestDecode / 完成回调 / clear）。
struct DecodeState
{
  QSet<QString> pending;
  quint64 epoch = 0;
};

DecodeState &decodeState()
{
  static DecodeState state;
  return state;
}

// 完成回调的投递目标。挂在 QCoreApplication 上，跟 GUI 线程走。
QObject *decodeHub()
{
  QCoreApplication *app = QCoreApplication::instance();
  if (!app || QThread::currentThread() != app->thread())
    return nullptr;
  static QObject *hub = new QObject(app);
  return hub;
}

} // namespace

void setFullLoadHook(std::function<void()> hook)
{
  g_fullLoadHook = std::move(hook);
}

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
  if (g_fullLoadHook)
    g_fullLoadHook();
  QImageReader reader(path);
  reader.setAutoTransform(true);
  return reader.read();
}

QImage FullImageCache::peek(const QString &key) const
{
  return m_map.value(key);
}

void FullImageCache::touch(const QString &key)
{
  const qsizetype idx = m_order.indexOf(key);
  if (idx >= 0)
    m_order.removeAt(idx);
  m_order.append(key);
}

QImage FullImageCache::acquire(const QString &key, const QString &path)
{
  const auto it = m_map.constFind(key);
  if (it != m_map.constEnd())
  {
    touch(key);
    return it.value();
  }
  const QImage img = loadFull(path);
  if (img.isNull())
    return img; // 装载失败不占额（下次触发重试——文件可能已恢复）
  store(key, img);
  return img;
}

void FullImageCache::store(const QString &key, const QImage &img)
{
  if (key.isEmpty() || img.isNull())
    return;
  m_map.insert(key, img);
  touch(key);
  while (m_order.size() > LodPolicy::kFullCacheEntries)
  {
    m_map.remove(m_order.front());
    m_order.removeFirst();
  }
}

bool FullImageCache::requestDecode(const QString &key, const QString &path,
                                   std::function<void(const QImage &)> done)
{
  if (key.isEmpty() || path.isEmpty() || !done)
    return false;
  QObject *hub = decodeHub();
  if (!hub)
    return false;
  DecodeState &st = decodeState();
  if (st.pending.contains(key))
    return false;
  st.pending.insert(key);
  const quint64 epoch = st.epoch;
  auto *watcher = new QFutureWatcher<QImage>(hub);
  // QueuedConnection：即使 future 已在 setFuture 时完成，也不在绘制栈里
  // 跑 done（done 会 fromImage / update）。
  QObject::connect(
      watcher, &QFutureWatcher<QImage>::finished, hub,
      [watcher, key, epoch, done = std::move(done)]() {
        const QImage img = watcher->result();
        watcher->deleteLater();
        DecodeState &state = decodeState();
        if (state.epoch != epoch)
          return; // clear() 已作废这一代；pending 已清空
        // 回调返回前保持 pending，重入绘制不会再排一次。
        done(img);
        if (state.epoch == epoch)
          state.pending.remove(key);
      },
      Qt::QueuedConnection);
  watcher->setFuture(QtConcurrent::run([path]() -> QImage {
    // 工作线程只产 QImage。QPixmap 与 FullImageCache 都不是线程安全的。
    return loadFull(path);
  }));
  return true;
}

bool FullImageCache::isDecodePending(const QString &key) const
{
  return decodeState().pending.contains(key);
}

void FullImageCache::clear()
{
  m_order.clear();
  m_map.clear();
  DecodeState &st = decodeState();
  st.pending.clear();
  ++st.epoch;
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
