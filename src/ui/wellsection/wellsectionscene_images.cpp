// 层：视图
// wellsectionscene_images — ColumnItem 图片道两级 LOD：缩略/原图 QPixmap
// 缓存（imageVersion 代际失效——版本不进键）+ 原图后台解码调度 + 绘制。
// 零改动拆分（方向 98；行为与缓存键语义逐字保留）。
#include "wellsectionscene.h"

#include "services/imagelod.h"

#include <QApplication>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QStyleOptionGraphicsItem>
#include <QThread>
#include <QTransform>

#include <QtNumeric>
#include <cmath>
#include <utility>

namespace wellsectionui {

void ColumnItem::noteFullPixmapUse(quint64 key)
{
  const qsizetype idx = m_fullPixmapOrder.indexOf(key);
  if (idx < 0 || idx + 1 == m_fullPixmapOrder.size())
    return;
  m_fullPixmapOrder.removeAt(idx);
  m_fullPixmapOrder.append(key);
}

void ColumnItem::retainFullPixmap(quint64 key, const QPixmap &pm)
{
  if (pm.isNull())
    return;
  m_pixmapCache.insert(key, pm);
  const qsizetype idx = m_fullPixmapOrder.indexOf(key);
  if (idx >= 0)
    m_fullPixmapOrder.removeAt(idx);
  m_fullPixmapOrder.append(key);
  const int cap = paleo::imagelod::LodPolicy::kFullCacheEntries;
  while (m_fullPixmapOrder.size() > cap)
  {
    // 优先淘汰当前不可见的原图；全都可见时丢最久未用的（不含刚插入的）。
    qsizetype evictAt = -1;
    for (qsizetype i = 0; i < m_fullPixmapOrder.size(); ++i)
    {
      const quint64 cand = m_fullPixmapOrder.at(i);
      if (cand == key)
        continue;
      if (!m_visibleFullKeys.contains(cand))
      {
        evictAt = i;
        break;
      }
      if (evictAt < 0)
        evictAt = i;
    }
    if (evictAt < 0)
      break;
    m_pixmapCache.remove(m_fullPixmapOrder.takeAt(evictAt));
  }
}

void ColumnItem::scheduleFullDecode(const QString &cacheKey, const QString &path,
                                    quint64 pixmapKey)
{
  if (!m_imageDecodeHost)
    return;
  QPointer<QGraphicsScene> sceneGuard(scene());
  const std::weak_ptr<ImageDecodeHost> host = m_imageDecodeHost;
  const quint64 version = m_st ? m_st->imageVersion : 0;
  paleo::imagelod::FullImageCache::shared().requestDecode(
      cacheKey, path,
      [host, sceneGuard, version, pixmapKey, cacheKey](const QImage &img) {
        const std::shared_ptr<ImageDecodeHost> locked = host.lock();
        if (!locked || !locked->self)
          return;
        ColumnItem *self = locked->self;
        if (img.isNull())
        {
          // 坏文件不占额，但本代际不再每帧重排（版本变了会清掉）。
          self->m_fullDecodeFailed.insert(cacheKey);
          return;
        }
        // QPixmap 只在 GUI、且场景和视图都还在、数据代际未变时生成。
        if (!sceneGuard || sceneGuard->views().isEmpty())
          return;
        if (self->scene() != sceneGuard.data())
          return;
        if (!self->m_st || self->m_st->imageVersion != version)
          return;
        Q_ASSERT(!qApp || QThread::currentThread() == qApp->thread());
        paleo::imagelod::FullImageCache::shared().store(cacheKey, img);
        const QPixmap pm = QPixmap::fromImage(img);
        self->retainFullPixmap(pixmapKey, pm);
        sceneGuard->update();
      });
}

// 图片道：core/lab_analysis 井附件照片按 depthMd 锚定。装载两级（方向 79
// LodPolicy，src/services/imagelod.h）：任务线程给缩略位图（≤256px 常驻），
// 视图线程转 QPixmap（imageVersion 代际失效——版本不进键，修掉旧 8 位截断
// 假命中）。可见宽（场景宽 × 视图缩放）超缩略宽 × kFullLoadFactor 时要
// 原图：只 peek，未命中后台解码；本函数不调用 loadFull / QImageReader /
// acquire。原图 QPixmap 最多 kFullCacheEntries 张。可见原图已占满上限时
// 其余锚这一帧继续画缩略，避免淘汰后又立刻重解码。透明图垫中性灰棋盘底；
// 缩小绘制开平滑变换（SmoothTransformation——minification 质量）。等比缩
// 到道宽、最小高 20px；绘制矩形留作双击拾取面。
void ColumnItem::paintImageTrack(QPainter *p, const QRectF &trackRect,
                                 const QRectF &exposed, const QTransform &world)
{
  p->save();
  p->setClipRect(trackRect);
  p->setRenderHint(QPainter::SmoothPixmapTransform, true);
  if (m_pixmapCacheVersion != m_st->imageVersion)
  {
    m_pixmapCache.clear();
    m_fullPixmapOrder.clear();
    m_fullDecodeFailed.clear();
    m_pixmapCacheVersion = m_st->imageVersion;
  }
  m_imageHits.clear();
  const wellsection::Well &w = m_st->wells.value(m_index);
  const double lod = world.isIdentity() ? 1.0 : world.m11();
  const double tw = trackRect.width() - 4.0;

  struct Slot
  {
    int index = -1;
    double y = 0;
    bool wantFull = false;
    quint64 key = 0;
    QString fullKey;
    QPixmap pm;
  };
  QVector<Slot> drawn;
  drawn.reserve(w.images.size());
  m_visibleFullKeys.clear();
  for (int i = 0; i < w.images.size(); ++i)
  {
    const double y = m_st->yForMd(m_index, w.images.at(i).md);
    if (y < exposed.top() - 80 || y > exposed.bottom() + 20)
      continue;
    // LOD 全载判定先行（可见设备宽 > 缩略宽 × 系数 → 原图级）；层级编进
    // 缓存键最高位——缩略级命中不得短路放大后的原图升级。
    const bool wantFull =
        !w.images.at(i).path.isEmpty() && lod > 1.0 &&
        tw * lod > w.images.at(i).image.width() *
                       paleo::imagelod::LodPolicy::kFullLoadFactor;
    Slot s;
    s.index = i;
    s.y = y;
    s.wantFull = wantFull;
    s.key = (quint64(quint32(m_index)) << 32) | quint64(quint32(i)) |
            (wantFull ? (1ULL << 63) : 0ULL);
    if (wantFull)
    {
      s.fullKey = QStringLiteral("%1|%2").arg(
          w.images.at(i).path, QString::number(m_st->imageVersion));
      m_visibleFullKeys.insert(s.key);
    }
    s.pm = m_pixmapCache.value(s.key);
    drawn.append(std::move(s));
  }
  for (const Slot &s : drawn)
  {
    if (s.wantFull && !s.pm.isNull())
      noteFullPixmapUse(s.key);
  }

  int slotsLeft = paleo::imagelod::LodPolicy::kFullCacheEntries;
  auto &fullCache = paleo::imagelod::FullImageCache::shared();
  for (const Slot &s : drawn)
  {
    if (!s.wantFull)
      continue;
    if (!s.pm.isNull() || fullCache.isDecodePending(s.fullKey))
      --slotsLeft;
  }
  slotsLeft = qMax(0, slotsLeft);
  for (Slot &s : drawn)
  {
    if (!s.wantFull || !s.pm.isNull())
      continue;
    if (s.fullKey.isEmpty() || m_fullDecodeFailed.contains(s.fullKey))
      continue;
    if (fullCache.isDecodePending(s.fullKey) || slotsLeft <= 0)
      continue;
    const QImage cached = fullCache.peek(s.fullKey);
    if (!cached.isNull())
    {
      // peek 不读盘。QPixmap 转换留在 GUI 线程（此处就是绘制线程）。
      s.pm = QPixmap::fromImage(cached);
      retainFullPixmap(s.key, s.pm);
      --slotsLeft;
      continue;
    }
    scheduleFullDecode(s.fullKey, w.images.at(s.index).path, s.key);
    --slotsLeft;
  }

  for (const Slot &s : drawn)
  {
    QPixmap pm = s.pm;
    const wellsection::ImageAnchor &anchor = w.images.at(s.index);
    if (pm.isNull() && !anchor.image.isNull())
    {
      // 原图未到：用井上已有缩略。不得写入原图键，否则放大后永不升级。
      const quint64 thumbKey = s.key & ~(1ULL << 63);
      pm = m_pixmapCache.value(thumbKey);
      if (pm.isNull())
      {
        pm = QPixmap::fromImage(anchor.image);
        if (!pm.isNull())
          m_pixmapCache.insert(thumbKey, pm);
      }
    }
    if (!pm.isNull())
    {
      const double ph = qMax(20.0, tw * pm.height() / double(pm.width()));
      const QRectF r(trackRect.left() + 2.0, s.y, tw, ph);
      if (pm.hasAlpha())
        p->fillRect(r, paleo::imagelod::alphaCheckerboard());
      p->drawPixmap(r, pm, pm.rect());
      p->setPen(QPen(m_st->theme.frame, 1.0));
      p->drawRect(r);
      m_imageHits.append({r, s.index});
    }
    else
    {
      // 无位图（装载失败/缺文件）：灰底占位如实示缺。
      const QRectF r(trackRect.left() + 2.0, s.y, tw, 20.0);
      p->fillRect(r, m_st->theme.paper);
      p->setPen(QPen(m_st->theme.frame, 1.0));
      p->drawRect(r);
      m_imageHits.append({r, s.index});
    }
  }
  p->restore();
}

} // namespace wellsectionui
