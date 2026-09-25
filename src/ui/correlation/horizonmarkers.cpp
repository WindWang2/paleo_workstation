#include "horizonmarkers.h"

#include "correlationwellcolumn.h"

#include <QCursor>
#include <QFont>
#include <QGraphicsLineItem>
#include <QGraphicsScene>
#include <QGraphicsSceneEvent>
#include <QGraphicsSimpleTextItem>
#include <QPainterPath>
#include <QPen>
#include <QTimer>

#include <cmath>
#include <functional>
#include <limits>

// ui/correlation/ — HorizonMarkerSet scene composition (subtask C).
//
// rebuild() draws one vertically-draggable line per (marker, picked
// column) plus one 8pt name label per marker anchored above the leftmost
// picked column. Every marker item carries
// CorrelationItemRoles::HorizonMarker so the next rebuild can reap them
// with a role scan — no pointers are kept between rebuilds, so nothing
// dangles whether the panel wiped the scene (scene->clear()) or left the
// items for us to replace.
//
// Drag safety: a dragged line commits its pick through a 0-timer on the
// set, never synchronously inside the item's mouse handler — a directly
// connected panel slot (full rebuild, even scene->clear()) could destroy
// the item while we are still on its stack. rebuild() additionally
// spares and updates the scene's current mouse grabber (the line that is
// mid-drag) so a drag survives the panel's per-commit rebuild.

namespace
{
  const QColor kDefaultMarkerColor(QStringLiteral("#F29900")); // DESIGN.md warning

  constexpr qreal kNormalZ = 0.0;   // marker lines sit on the column baseline
  constexpr qreal kActiveZ = 1.0;   // active horizon: one Z level above siblings
  constexpr qreal kLabelZ = 2.0;    // labels must read above every marker line
  constexpr qreal kGrabSlack = 4.0; // grab tolerance band around a 1.5px line
  constexpr qreal kLabelGap = 2.0;  // label floats this far above its line

  const float kNaNf = std::numeric_limits<float>::quiet_NaN();
}

// Vertically draggable marker line.
//
// Drag strategy: we take over the mouse events ourselves instead of
// setting QGraphicsItem::ItemIsMovable — the built-in move follows the
// cursor on BOTH axes and would need an ItemChange clamp on every step,
// while a hand-rolled move hard-locks x to the column span and only
// tracks y, the single degree of freedom a pick depth has.
class MarkerLineItem : public QGraphicsLineItem
{
  public:
    MarkerLineItem(HorizonMarkerSet *set, const QString &marker, const QString &wellId)
      : m_set(set), m_marker(marker), m_wellId(wellId) {}

    // (Re)applies the per-rebuild interaction config — reusable across
    // rebuilds for the line that is preserved mid-drag.
    void configure(const std::function<float(qreal)> &depthFor, bool editable)
    {
      m_depthFor = depthFor;
      m_editable = editable;
      // Hover cursor only where a drag actually exists.
      setAcceptHoverEvents(editable);
      setAcceptedMouseButtons(editable ? Qt::LeftButton : Qt::NoButton);
      if (!editable)
        unsetCursor();
    }

    void mousePressEvent(QGraphicsSceneMouseEvent *e) override
    {
      if (!m_editable || e->button() != Qt::LeftButton)
      {
        e->ignore();
        return;
      }
      // Anchor the drag on the line, not the cursor: pressing within the
      // grab slack must not snap the line onto the cursor.
      m_pressLineY = lineSceneY();
      m_pressDy = e->scenePos().y() - m_pressLineY;
      m_pressPick = m_set ? m_set->wellDepth(m_marker, m_wellId) : kNaNf;
      e->accept();
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent *e) override
    {
      if (!m_editable || !(e->buttons() & Qt::LeftButton) || std::isnan(m_pressPick))
      {
        e->ignore();
        return;
      }
      // Line-anchored cursor y (press offset removed).
      const qreal targetY = e->scenePos().y() - m_pressDy;
      if (!pinnedByFlatten())
      {
        const qreal y = mapFromScene(QPointF(0, targetY)).y();
        const QLineF cur = line();
        setLine(cur.x1(), y, cur.x2(), y); // x stays locked to the column span
      }
      commitDelta(targetY);
      e->accept();
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent *e) override { e->accept(); }

    void hoverEnterEvent(QGraphicsSceneHoverEvent *) override
    {
      if (m_editable)
        setCursor(QCursor(Qt::SizeVerCursor)); // vertical-only affordance
    }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *) override
    {
      if (m_editable)
        unsetCursor();
    }

    // A 1.5px line is humanly ungrabbable: widen the interaction shape.
    QPainterPath shape() const override
    {
      const QLineF l = line();
      QPainterPath p;
      p.addRect(qMin(l.x1(), l.x2()), qMin(l.y1(), l.y2()) - kGrabSlack,
                qMax(qAbs(l.dx()), 1.0), qAbs(l.dy()) + 2 * kGrabSlack);
      return p;
    }

  private:
    qreal lineSceneY() const { return mapToScene(QPointF(0, line().y1())).y(); }

    // Flattening pins the flatten marker's own line at display depth 0:
    // dragging it then shifts the well's DATA (offset), not the line.
    bool pinnedByFlatten() const
    {
      return m_set && m_set->isFlattened() && m_set->flattenMarker() == m_marker;
    }

    void commitDelta(qreal targetY)
    {
      if (!m_set || !m_depthFor)
        return;
      // Depth delta measured through the panel's (possibly non-linear)
      // y<->depth mapping, from the line's position at press to now.
      const float delta = m_depthFor(targetY) - m_depthFor(m_pressLineY);
      if (std::isnan(delta))
        return;
      const float pick = m_pressPick + delta;
      // Deferred commit: emitting markerDepthChanged synchronously from in
      // here could let a directly connected panel rebuild (even
      // scene->clear()) delete this item while we are still on its stack.
      // The 0-timer keeps the emit off the item; rebuild() preserves the
      // grabbed line so the drag continues seamlessly after the rebuild.
      QTimer::singleShot(0, m_set, [set = m_set, marker = m_marker,
                                    well = m_wellId, pick]() {
        set->setWellDepth(marker, well, pick);
      });
    }

    HorizonMarkerSet *m_set = nullptr;
    QString m_marker;
    QString m_wellId;
    std::function<float(qreal)> m_depthFor;
    bool m_editable = false;
    qreal m_pressDy = 0.0;
    qreal m_pressLineY = 0.0;
    float m_pressPick = kNaNf;
};

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
  if (!scene)
    return;

  // Reap the previous marker items (lines AND labels) by role. If the
  // panel already cleared the scene the scan finds nothing — no pointers
  // are stored across rebuilds, so nothing can dangle. The item under an
  // in-flight drag (the scene's mouse grabber) is spared and updated in
  // place below so the drag survives this rebuild.
  QGraphicsItem *grabber = scene->mouseGrabberItem();
  const QList<QGraphicsItem *> existing = scene->items();
  for (QGraphicsItem *it : existing)
  {
    if (it == grabber)
      continue;
    if (it->data(CorrelationItemRoles::HorizonMarker).isValid())
      delete it;
  }

  if (!yFor)
    return;

  // Items hang under `parent` (keeping the panel's column items the only
  // top-level ones); a parent that is not in this scene could never be
  // reaped again, so top-level hosting is forced instead.
  QGraphicsItem *const host = (parent && parent->scene() == scene) ? parent : nullptr;

  // Column rects arrive in scene px; items live in `host` coordinates
  // (QGraphicsItem has no rect-mapping overload — map the corners).
  const auto rectInHost = [host](const QRectF &r) {
    return host ? QRectF(host->mapFromScene(r.topLeft()),
                         host->mapFromScene(r.bottomRight())).normalized()
                : r;
  };
  const auto yInHost = [host](qreal sceneY) {
    return host ? host->mapFromScene(QPointF(0, sceneY)).y() : sceneY;
  };

  bool grabberReused = false;
  for (const Marker &m : m_markers)
  {
    const bool active = (m.name == activeHorizon);
    const QPen pen(m.color, active ? 2.5 : 1.5); // active: thicker + one Z up

    // Label anchor: the leftmost column carrying a pick for this marker.
    bool anyPick = false;
    qreal labelSceneY = 0.0;
    QRectF labelSceneRect;

    for (const ColumnGeom &g : geoms)
    {
      const float pick = m.depthByWell.value(g.wellId, kNaNf);
      if (std::isnan(pick))
        continue;
      const qreal sceneY = yFor(displayDepth(g.wellId, pick)); // display space

      MarkerLineItem *line = nullptr;
      if (grabber && !grabberReused
          && grabber->data(CorrelationItemRoles::HorizonMarker).toString() == m.name
          && grabber->data(CorrelationItemRoles::WellId).toString() == g.wellId)
      {
        // The line mid-drag: keep this exact item (and its grab). Only
        // our lines accept buttons among role-carrying items, so the
        // downcast is safe.
        line = static_cast<MarkerLineItem *>(
            qgraphicsitem_cast<QGraphicsLineItem *>(grabber));
        if (line->parentItem() != host)
          line->setParentItem(host);
        grabberReused = true;
      }
      else
      {
        line = new MarkerLineItem(this, m.name, g.wellId);
        line->setParentItem(host);
        if (!host)
          scene->addItem(line);
        line->setData(CorrelationItemRoles::HorizonMarker, m.name);
        line->setData(CorrelationItemRoles::WellId, g.wellId);
      }
      const QRectF r = rectInHost(g.rect);
      const qreal y = yInHost(sceneY);
      line->configure(depthFor, editable);
      line->setLine(r.left(), y, r.right(), y); // spans its whole column
      line->setPen(pen);
      line->setZValue(active ? kActiveZ : kNormalZ);

      if (!anyPick || g.rect.left() < labelSceneRect.left())
      {
        anyPick = true;
        labelSceneRect = g.rect;
        labelSceneY = sceneY;
      }
    }

    if (anyPick)
    {
      // ONE label per marker — readable across the whole section, not
      // one per column. Mouse-transparent; colored like its data symbol.
      auto *label = new QGraphicsSimpleTextItem(m.name, host);
      if (!host)
        scene->addItem(label);
      QFont f = label->font();
      f.setPointSizeF(8.0); // DESIGN.md label size
      label->setFont(f);
      label->setBrush(m.color);
      label->setData(CorrelationItemRoles::HorizonMarker, m.name); // reap key
      label->setAcceptedMouseButtons(Qt::NoButton);
      label->setZValue(kLabelZ);
      const QRectF r = rectInHost(labelSceneRect);
      label->setPos(r.left(),
                    yInHost(labelSceneY) - label->boundingRect().height() - kLabelGap);
    }
  }

  // The spared drag line's (marker, well) produced no item above — e.g.
  // its pick disappeared — retire it now that no event handler of it can
  // be on the stack (drag commits are deferred to the event loop).
  if (grabber && !grabberReused && grabber->scene() == scene)
    delete grabber;
}

float HorizonMarkerSet::lineDepthAt(const QString &marker, const QString &wellId) const
{
  // Model-derived, item-free: exactly the display depth rebuild() fed
  // yFor for this marker/well, so callers can assert scene geometry
  // without touching items. NaN when there is no pick (no line drawn).
  const float pick = wellDepth(marker, wellId);
  return std::isnan(pick) ? pick : displayDepth(wellId, pick);
}
