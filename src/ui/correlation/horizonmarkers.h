// 层：视图
#pragma once
#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QRectF>
#include <QString>
#include <QStringList>

#include <functional>

class QGraphicsItem;
class QGraphicsScene;

// ui/correlation/ — HorizonMarkerSet: named stratigraphic markers drawn as
// correlation lines crossing every well column at per-well picked depths,
// plus the flatten-on-marker transform.
//
// Data sources (panel feeds both): horizons declared in the manifest
// (horizon=* LayerDeclarations) seed the marker set; SelectionContext's
// activeHorizon designates the marker to highlight/flatten.
//
// Flatten-on-marker geometry: with marker M flattened, a sample at real
// depth d in well w displays at (d - pick(w)) — i.e. every well's M-pick
// sits at display depth 0, so M renders as ONE horizontal line across the
// section and all other geometry keeps its per-well relative position.
// Wells without a pick for M keep the identity mapping (unflattened).
class HorizonMarkerSet : public QObject
{
  Q_OBJECT
  public:
    explicit HorizonMarkerSet(QObject *parent = nullptr);

    struct Marker
    {
      QString name;
      QColor color;                       // data symbol — default orange #F29900
      QHash<QString, float> depthByWell;  // well id → pick depth (NaN = no pick)
    };

    // --- model --------------------------------------------------------------
    // Manifest-declared horizon names: adds markers for names not yet
    // present (default color), keeps picks of existing ones, and REMOVES
    // markers that are no longer declared (declared set is authoritative).
    void setManifestHorizons(const QStringList &names);
    QStringList markerNames() const;
    bool hasMarker(const QString &name) const;
    bool addMarker(const QString &name, const QColor &color = QColor());
    bool removeMarker(const QString &name);
    void setMarkerColor(const QString &name, const QColor &color);
    QColor markerColor(const QString &name) const;

    // Per-well picks. Setting the first pick of the flatten marker does
    // NOT auto-flatten — the panel switches modes explicitly.
    void setWellDepth(const QString &marker, const QString &wellId, float depth);
    float wellDepth(const QString &marker, const QString &wellId) const; // NaN if none
    QStringList wellsPicked(const QString &marker) const;

    // --- flatten transform ---------------------------------------------------
    void setFlattenMarker(const QString &name);   // {} / unknown = off
    QString flattenMarker() const { return m_flatten; }
    bool isFlattened() const { return !m_flatten.isEmpty(); }
    // display = real - pick(well); identity when not flattened or the
    // well has no pick.
    float displayDepth(const QString &wellId, float realDepth) const;
    // The offset to subtract for `wellId` (0 when identity) — the panel
    // feeds this straight into CorrelationTrack::render's depthOffset.
    float displayOffset(const QString &wellId) const;

    // --- scene composition ----------------------------------------------------
    // One labeled line per marker × picked well, drawn across the given
    // column rectangles. yFor maps a DISPLAY depth to scene y (the panel
    // owns the axis mapping); depthFor inverts it for drag-back-conversion.
    // editable=true makes lines draggable vertically (dragging adjusts the
    // drag-well's pick and emits markerDepthChanged). Items parent to
    // `parent` so the panel keeps exactly its column items top-level.
    struct ColumnGeom
    {
      QString wellId;
      QRectF rect;                        // the column's body rect in scene px
    };
    void rebuild(QGraphicsScene *scene, QGraphicsItem *parent,
                 const QList<ColumnGeom> &geoms,
                 const std::function<qreal(float)> &yFor,
                 const std::function<float(qreal)> &depthFor,
                 bool editable, const QString &activeHorizon = QString());

    // Marker line at display depth `displayDepth` inside column `wellId`
    // (drag target lookup, tests). NaN when the marker has no pick there.
    float lineDepthAt(const QString &marker, const QString &wellId) const;

  signals:
    // A line was dragged (or a pick set programmatically) — the panel
    // re-renders affected wells (flatten mode makes it all of them).
    void markerDepthChanged(const QString &marker, const QString &wellId, float depth);

  private:
    QList<Marker> m_markers;               // declaration order
    QString m_flatten;
};
