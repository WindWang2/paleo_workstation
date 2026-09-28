// 层：视图
#include "correlationwellcolumn.h"
#include "../paleotheme.h"

#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QPainterPath>
#include <QPen>
#include <QSizeF>
#include <QTransform>

#include <cstring>

namespace
{
  // 64-bit cache key over the full render-identity tuple: (mnemonic, strip
  // size, section axis, flatten offset, stroke color, dataRevision). The
  // dataRevision component is a CONTENT FINGERPRINT over the sample bits:
  // the column has no hook into CorrelationTrack::setData, so deriving the
  // revision from the data itself makes addTrack-replacement AND direct
  // track()->setData() mutations both change the key — a stale track is
  // never served. qHashBits over 2×N floats is a few µs per track, noise
  // next to the QgsLineChartPlot render it guards on 50-well rebuilds.
  quint64 trackCacheKey(const CorrelationTrack &t, int stripW, int stripH,
                        float depthMin, float depthMax, float depthOffset)
  {
    size_t h = qHash(t.mnemonic());
    h = qHash(stripW, h);
    h = qHash(stripH, h);
    const auto mixFloat = [&h](float v) {
      quint32 bits = 0;
      std::memcpy(&bits, &v, sizeof(bits)); // bit-exact float identity
      h = qHash(bits, h);
    };
    mixFloat(depthMin);
    mixFloat(depthMax);
    mixFloat(depthOffset);
    h = qHash(t.color().rgba(), h);
    h = qHash(int(t.sampleCount()), h);
    const QVector<float> &depths = t.depths();
    if (!depths.isEmpty())
      h = qHashBits(depths.constData(), qsizetype(depths.size() * sizeof(float)), h);
    const QVector<float> &values = t.values();
    if (!values.isEmpty())
      h = qHashBits(values.constData(), qsizetype(values.size() * sizeof(float)), h);
    return static_cast<quint64>(h);
  }
} // namespace

CorrelationWellColumn::CorrelationWellColumn(const QString &wellId, const QString &wellName)
  : m_wellId(wellId), m_wellName(wellName)
{
}

bool CorrelationWellColumn::addTrack(const CorrelationTrack &track)
{
  const QString key = track.mnemonic();
  if (key.isEmpty())
    return false;
  const auto it = m_index.constFind(key);
  if (it != m_index.constEnd())
  {
    m_tracks[it.value()] = track;
    m_cache.remove(key); // replaced data is a guaranteed miss (key also moves)
    return true;
  }
  m_tracks.append(track);
  m_index.insert(key, m_tracks.size() - 1);
  return true;
}

bool CorrelationWellColumn::removeTrack(const QString &mnemonic)
{
  const auto it = m_index.constFind(mnemonic);
  if (it == m_index.constEnd())
    return false;
  m_tracks.removeAt(it.value());
  m_cache.remove(mnemonic);
  m_index.clear();
  for (int i = 0; i < m_tracks.size(); ++i)
    m_index.insert(m_tracks.at(i).mnemonic(), i);
  return true;
}

void CorrelationWellColumn::clearTracks()
{
  m_tracks.clear();
  m_index.clear();
  invalidateCache();
}

CorrelationTrack *CorrelationWellColumn::track(const QString &mnemonic)
{
  const auto it = m_index.constFind(mnemonic);
  return (it != m_index.constEnd()) ? &m_tracks[it.value()] : nullptr;
}

const CorrelationTrack *CorrelationWellColumn::track(const QString &mnemonic) const
{
  const auto it = m_index.constFind(mnemonic);
  return (it != m_index.constEnd()) ? &m_tracks.at(it.value()) : nullptr;
}

bool CorrelationWellColumn::hasTrack(const QString &mnemonic) const
{
  return m_index.contains(mnemonic);
}

int CorrelationWellColumn::trackCount() const
{
  return m_tracks.size();
}

QStringList CorrelationWellColumn::mnemonics() const
{
  QStringList out;
  out.reserve(m_tracks.size());
  for (const CorrelationTrack &t : m_tracks)
    out << t.mnemonic();
  return out;
}

void CorrelationWellColumn::setTrackWidth(qreal px)
{
  if (px >= 20.0)
    m_trackWidth = px;
}

qreal CorrelationWellColumn::width() const
{
  if (m_tracks.isEmpty())
    return m_trackWidth; // empty column keeps one strip wide
  return m_tracks.size() * m_trackWidth;
}

void CorrelationWellColumn::invalidateCache()
{
  m_cache.clear();
  m_cacheHits = 0;
}

QGraphicsPathItem *CorrelationWellColumn::rebuild(QGraphicsScene *scene, const QPointF &topLeft,
                                                  qreal bodyHeight, float depthMin,
                                                  float depthMax, bool highlighted,
                                                  float depthOffset)
{
  if (!scene)
    return nullptr;

  // Column chrome: 4px rounded white card; border token, or the primary
  // selected pen when highlighted (DESIGN.md § selected state).
  const QColor border = PaleoTheme::tokens().border;
  const QColor primary = PaleoTheme::tokens().primary;
  QPainterPath path;
  path.addRoundedRect(QRectF(topLeft, QSizeF(width(), m_headerHeight + bodyHeight)), 4.0, 4.0);
  auto *column = scene->addPath(path, highlighted ? QPen(primary, 2.0) : QPen(border, 1.0),
                                QBrush(PaleoTheme::tokens().surface));
  column->setData(CorrelationItemRoles::WellId, m_wellId);
  column->setData(CorrelationItemRoles::Highlight, highlighted);

  // Well name header: 9pt body token, centered in the header band.
  auto *label = new QGraphicsSimpleTextItem(m_wellName, column);
  QFont f = label->font();
  f.setPointSize(9);
  label->setFont(f);
  label->setBrush(PaleoTheme::tokens().text);
  const QRectF lb = label->boundingRect();
  label->setPos(topLeft.x() + (width() - lb.width()) / 2.0,
                topLeft.y() + (m_headerHeight - lb.height()) / 2.0);

  if (!m_tracks.isEmpty() && bodyHeight > 0.0)
  {
    // Track strips: N vertical strips side-by-side below the header band,
    // each trackWidth wide × bodyHeight tall, all sharing the section axis.
    const int stripW = qMax(1, qRound(m_trackWidth));
    const int stripH = qMax(1, qRound(bodyHeight));
    const qreal bodyTop = topLeft.y() + m_headerHeight;

    for (int i = 0; i < m_tracks.size(); ++i)
    {
      const CorrelationTrack &t = m_tracks.at(i);
      const qreal stripX = topLeft.x() + i * m_trackWidth;

      // Cache lookup: a stored entry only hits when the WHOLE identity key
      // matches (geometry, axis, offset, color, data fingerprint) — a hit
      // skips render() entirely; a miss re-renders and re-stores.
      const quint64 key = trackCacheKey(t, stripW, stripH, depthMin, depthMax, depthOffset);
      QPixmap pmData;
      const auto cit = m_cache.constFind(t.mnemonic());
      if (cit != m_cache.constEnd() && cit.value().key == key)
      {
        pmData = cit.value().pixmap;
        ++m_cacheHits;
      }
      else
      {
        const QImage img = t.render(stripW, stripH, depthMin, depthMax, depthOffset);
        if (!img.isNull())
          pmData = QPixmap::fromImage(img);
        m_cache.insert(t.mnemonic(), CachedImage{key, img, pmData});
      }

      // Null pixmap (empty track / stub renderer): the pixmap is skipped,
      // but caption and separators still draw — the column chrome never
      // depends on render succeeding.
      if (!pmData.isNull())
      {
        auto *pm = new QGraphicsPixmapItem(pmData, column);
        pm->setPos(stripX, bodyTop);
        // Stretch the (possibly rounded-size) image to the full strip —
        // pos+fromScale maps the pixmap rect onto exactly [stripX, stripX+
        // trackWidth]×[bodyTop, bodyTop+bodyHeight] (verified composition).
        const qreal sx = m_trackWidth / pmData.width();
        const qreal sy = bodyHeight / pmData.height();
        if (!qFuzzyCompare(sx, 1.0) || !qFuzzyCompare(sy, 1.0))
          pm->setTransform(QTransform::fromScale(sx, sy));
        pm->setAcceptedMouseButtons(Qt::NoButton); // clicks resolve to the well
        pm->setData(CorrelationItemRoles::CurveOwner, m_wellId);
        pm->setData(CorrelationItemRoles::TrackMnemonic, t.mnemonic());
      }

      // Track caption: the MNEMONIC as its own text item (legacy tests walk
      // text children for exactly "GR"), with the unit as a smaller sibling
      // beside it — both 8pt label token, top of the strip, children of the
      // column item.
      auto *cap = new QGraphicsSimpleTextItem(t.mnemonic(), column);
      QFont cf = cap->font();
      cf.setPointSize(8);
      cap->setFont(cf);
      cap->setBrush(PaleoTheme::tokens().textMuted);
      const QRectF cb = cap->boundingRect();
      const qreal unitGap = t.unit().isEmpty() ? 0.0 : cb.width() + 3.0;
      const qreal capX = stripX + (m_trackWidth - cb.width() - unitGap) / 2.0;
      cap->setPos(qMax(topLeft.x(), qMin(capX, topLeft.x() + width() - cb.width())),
                  bodyTop + 2.0);
      if (!t.unit().isEmpty())
      {
        auto *uc = new QGraphicsSimpleTextItem(t.unit(), column);
        QFont uf = uc->font();
        uf.setPointSize(7);
        uc->setFont(uf);
        uc->setBrush(PaleoTheme::tokens().textDisabled); // text-disabled as a soft unit tag
        uc->setPos(cap->x() + cb.width() + 3.0, bodyTop + 2.5);
      }

      // 1px border-token separator before every track but the first.
      if (i > 0)
      {
        auto *sep =
            new QGraphicsLineItem(stripX, bodyTop, stripX, bodyTop + bodyHeight, column);
        sep->setPen(QPen(border, 1.0));
      }
    }
  }
  return column;
}
