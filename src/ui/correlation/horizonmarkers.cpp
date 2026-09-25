#include "horizonmarkers.h"

#include "correlationwellcolumn.h"

#include <QGraphicsLineItem>
#include <QGraphicsScene>

#include <cmath>
#include <limits>

// Wave-base stub: model + flatten transform are final-quality; rebuild()
// draws bare lines — subtask C lands labels, drag interaction, active-
// horizon highlight and grid-spanning polish.

namespace
{
  const QColor kDefaultMarkerColor(QStringLiteral("#F29900")); // DESIGN.md warning
}

HorizonMarkerSet::HorizonMarkerSet(QObject *parent)
  : QObject(parent)
{
}

void HorizonMarkerSet::setManifestHorizons(const QStringList &names)
{
  // Declared set is authoritative: add missing, drop undeclared, keep picks.
  QList<Marker> next;
  for (const QString &n : names)
  {
    bool kept = false;
    for (const Marker &m : m_markers)
      if (m.name == n)
      {
        next.append(m);
        kept = true;
        break;
      }
    if (!kept)
      next.append({n, kDefaultMarkerColor, {}});
  }
  m_markers = next;
  if (!m_flatten.isEmpty() && !names.contains(m_flatten))
    m_flatten.clear();
}

QStringList HorizonMarkerSet::markerNames() const
{
  QStringList out;
  for (const Marker &m : m_markers)
    out << m.name;
  return out;
}

bool HorizonMarkerSet::hasMarker(const QString &name) const
{
  for (const Marker &m : m_markers)
    if (m.name == name)
      return true;
  return false;
}

bool HorizonMarkerSet::addMarker(const QString &name, const QColor &color)
{
  if (name.isEmpty() || hasMarker(name))
    return false;
  m_markers.append({name, color.isValid() ? color : kDefaultMarkerColor, {}});
  return true;
}

bool HorizonMarkerSet::removeMarker(const QString &name)
{
  for (int i = 0; i < m_markers.size(); ++i)
    if (m_markers.at(i).name == name)
    {
      m_markers.removeAt(i);
      if (m_flatten == name)
        m_flatten.clear();
      return true;
    }
  return false;
}

void HorizonMarkerSet::setMarkerColor(const QString &name, const QColor &color)
{
  for (Marker &m : m_markers)
    if (m.name == name)
    {
      m.color = color;
      return;
    }
}

QColor HorizonMarkerSet::markerColor(const QString &name) const
{
  for (const Marker &m : m_markers)
    if (m.name == name)
      return m.color;
  return {};
}

void HorizonMarkerSet::setWellDepth(const QString &marker, const QString &wellId, float depth)
{
  for (Marker &m : m_markers)
    if (m.name == marker)
    {
      m.depthByWell.insert(wellId, depth);
      emit markerDepthChanged(marker, wellId, depth);
      return;
    }
}

float HorizonMarkerSet::wellDepth(const QString &marker, const QString &wellId) const
{
  for (const Marker &m : m_markers)
    if (m.name == marker)
      return m.depthByWell.value(wellId, std::numeric_limits<float>::quiet_NaN());
  return std::numeric_limits<float>::quiet_NaN();
}

QStringList HorizonMarkerSet::wellsPicked(const QString &marker) const
{
  QStringList out;
  for (const Marker &m : m_markers)
    if (m.name == marker)
    {
      out = m.depthByWell.keys();
      std::sort(out.begin(), out.end());
      return out;
    }
  return out;
}

void HorizonMarkerSet::setFlattenMarker(const QString &name)
{
  m_flatten = (name.isEmpty() || !hasMarker(name)) ? QString() : name;
}

float HorizonMarkerSet::displayOffset(const QString &wellId) const
{
  if (m_flatten.isEmpty())
    return 0.0f;
  const float pick = wellDepth(m_flatten, wellId);
  return std::isnan(pick) ? 0.0f : pick;
}

float HorizonMarkerSet::displayDepth(const QString &wellId, float realDepth) const
{
  return realDepth - displayOffset(wellId);
}

void HorizonMarkerSet::rebuild(QGraphicsScene *scene, QGraphicsItem *parent,
                               const QList<ColumnGeom> &geoms,
                               const std::function<qreal(float)> &yFor,
                               const std::function<float(qreal)> &depthFor,
                               bool editable, const QString &activeHorizon)
{
  Q_UNUSED(depthFor); Q_UNUSED(editable); Q_UNUSED(activeHorizon);
  if (!scene)
    return;
  for (const Marker &m : m_markers)
  {
    for (const ColumnGeom &g : geoms)
    {
      const float d = displayDepth(g.wellId,
                                   m.depthByWell.value(g.wellId,
                                                       std::numeric_limits<float>::quiet_NaN()));
      if (std::isnan(d))
        continue;
      auto *line = new QGraphicsLineItem(g.rect.left(), yFor(d), g.rect.right(), yFor(d), parent);
      line->setPen(QPen(m.color, 1.5));
      line->setData(CorrelationItemRoles::HorizonMarker, m.name);
    }
  }
}

float HorizonMarkerSet::lineDepthAt(const QString &marker, const QString &wellId) const
{
  Q_UNUSED(marker); Q_UNUSED(wellId);
  return std::numeric_limits<float>::quiet_NaN(); // stub: subtask C implements via items
}
