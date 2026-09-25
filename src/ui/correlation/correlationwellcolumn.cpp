#include "correlationwellcolumn.h"

#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QPainterPath>
#include <QSizeF>

// Wave-base stub: model + geometry are final-quality; rebuild() draws only
// the column chrome (border + header) — track pixmaps land with subtask A2.

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
    m_cache.remove(key);
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
  Q_UNUSED(depthMin); Q_UNUSED(depthMax); Q_UNUSED(depthOffset);
  if (!scene)
    return nullptr;

  const QColor border(QStringLiteral("#DFE5EC"));
  const QColor primary(QStringLiteral("#1B73D0"));
  QPainterPath path;
  path.addRoundedRect(QRectF(topLeft, QSizeF(width(), m_headerHeight + bodyHeight)), 4.0, 4.0);
  auto *column = scene->addPath(path, highlighted ? QPen(primary, 2.0) : QPen(border, 1.0),
                                QBrush(Qt::white));
  column->setData(CorrelationItemRoles::WellId, m_wellId);
  column->setData(CorrelationItemRoles::Highlight, highlighted);

  auto *label = new QGraphicsSimpleTextItem(m_wellName, column);
  QFont f = label->font();
  f.setPointSize(9);
  label->setFont(f);
  label->setBrush(QColor(QStringLiteral("#24303E")));
  const QRectF lb = label->boundingRect();
  label->setPos(topLeft.x() + (width() - lb.width()) / 2.0,
                topLeft.y() + m_headerHeight - lb.height() - 2.0);
  return column; // stub: track strips/captions arrive with subtask A2
}
