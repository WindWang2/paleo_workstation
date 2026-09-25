#pragma once
#include <QColor>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QString>
#include <QStringList>

#include "correlationtrack.h"

class QGraphicsPathItem;
class QGraphicsScene;

// ui/correlation/ — CorrelationWellColumn: one well's column in the
// correlation section: a strip of N curve tracks sharing one depth axis,
// under a header band (well name), with column-level selection highlight.
//
// Track arrangement tradeoff: tracks sit SIDE-BY-SIDE horizontally (the
// industry-standard well-section layout — Techlog/Petrel render each curve
// as a full-height vertical strip). The alternative, stacking tracks
// vertically, was rejected: it breaks the continuous shared depth ruler
// running down the page and wastes the section's horizontal extent, which
// exists precisely to host several curves per well.
//
// Not a QWidget: the section is one QGraphicsScene (50-well sections with
// nested proxy widgets are unaffordable); this class composes plain
// graphics items and stays compatible with the click-resolution data
// roles the panel's scene already uses.
namespace CorrelationItemRoles
{
  constexpr int WellId = 0;        // item → owning well id (click resolution)
  constexpr int Highlight = 1;     // item → bool selected state
  constexpr int CurveOwner = 2;    // track pixmap item → owning well id
  constexpr int TrackMnemonic = 3; // track pixmap item → mnemonic
  constexpr int HorizonMarker = 4; // marker line item → marker name
  constexpr int MarkerWellId = 5;  // marker line → well id (not a column)
}

class CorrelationWellColumn
{
  public:
    explicit CorrelationWellColumn(const QString &wellId, const QString &wellName);

    QString wellId() const { return m_wellId; }
    QString wellName() const { return m_wellName; }

    // --- tracks -------------------------------------------------------------
    // Adds (or replaces, same mnemonic) a track. Mnemonics are unique per
    // column; addTrack returns false for an empty mnemonic.
    bool addTrack(const CorrelationTrack &track);
    bool removeTrack(const QString &mnemonic);
    void clearTracks();
    CorrelationTrack *track(const QString &mnemonic);
    const CorrelationTrack *track(const QString &mnemonic) const;
    bool hasTrack(const QString &mnemonic) const;
    int trackCount() const;
    QStringList mnemonics() const;            // in insertion order

    // --- geometry -----------------------------------------------------------
    // Column width in scene px: margin + N×trackWidth + separators.
    void setTrackWidth(qreal px);             // uniform strip width (default 90)
    qreal trackWidth() const { return m_trackWidth; }
    void setHeaderHeight(qreal px) { m_headerHeight = px; }
    qreal headerHeight() const { return m_headerHeight; }
    qreal width() const;

    // --- scene composition --------------------------------------------------
    // (Re)builds the column's items into `scene` (previous items were
    // deleted by the panel's scene->clear()) at topLeft, occupying the
    // given body height below the header band. depthMin/depthMax is the
    // shared section axis; depthOffset shifts this well's display depth
    // (flatten-on-marker, see CorrelationTrack::render). highlighted draws
    // the primary selection pen (DESIGN.md selected state). Returns the
    // column's top-level path item.
    QGraphicsPathItem *rebuild(QGraphicsScene *scene, const QPointF &topLeft,
                               qreal bodyHeight, float depthMin, float depthMax,
                               bool highlighted, float depthOffset = 0.0f);

    // Rendered track images are expensive on 50-well sections: the column
    // caches per-track QImages keyed by (mnemonic, size, axis, offset,
    // color) so reorder/highlight rebuilds reuse them; only real data or
    // geometry changes re-render. Test hooks:
    int cacheHits() const { return m_cacheHits; }
    void resetCacheStats() { m_cacheHits = 0; }
    void invalidateCache();                    // data/axis changed → drop all

  private:
    struct CachedImage
    {
      quint64 key = 0;
      QImage image;
    };

    QString m_wellId;
    QString m_wellName;
    QList<CorrelationTrack> m_tracks;
    QHash<QString, int> m_index;               // mnemonic → m_tracks position
    QHash<QString, CachedImage> m_cache;       // mnemonic → rendered image
    qreal m_trackWidth = 90.0;
    qreal m_headerHeight = 24.0;
    int m_cacheHits = 0;
};
