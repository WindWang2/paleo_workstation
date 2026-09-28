// 层：视图
#pragma once
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "correlation/horizonmarkers.h"

#include <functional>

// ui/ — WellCorrelationPanel: 连井剖面 (well correlation section).
//
// The constraint page's linked sub-panel: an ordered strip of well columns
// tied to map selection through SelectionContext (§41.3). Each column
// hosts any number of curve tracks (side-by-side strips sharing one
// section depth axis); curve bodies render through QgsLineChartPlot inside
// CorrelationTrack — the panel never hand-builds polylines. Section chrome
// (depth ruler, horizon correlation lines) is composed here: the ruler is
// painted by the view's background/foreground passes (NOT a scene item, so
// the scene's top-level items remain exactly the well columns — the click
// resolution and test scaffolding depend on that), while horizon lines
// live under a chrome parent item created only when markers are visible.
//
// The panel emits intents (wellClicked/wellDoubleClicked) and follows
// selectionChanged broadcasts whose origin isn't "correlation" (echo
// guard). Pure Qt Widgets chrome + QGIS plot rendering for curve bodies.
class QLabel;
class QGraphicsPathItem;
class QGraphicsRectItem;
class QGraphicsScene;
class QGraphicsView;
class SelectionContext;
class CorrelationWellColumn;
class CurveBrowser;
class DepthRuler;
class HorizonMarkerSet;
struct LasCurve;

class WellCorrelationPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit WellCorrelationPanel(SelectionContext *ctx, QWidget *parent = nullptr);
    ~WellCorrelationPanel() override;

    // Batching: setUpdatesEnabled(false) defers rebuildScene() until enabled.
    void setUpdatesEnabled(bool enabled);
    void beginBatch() { setUpdatesEnabled(false); }
    void endBatch() { setUpdatesEnabled(true); }

    // (id, name) pairs in section order; re-layouts the scene. Track sets
    // of wells that survive the replacement are kept.
    void setWells(const QList<QPair<QString, QString>> &wells);
    int wellCount() const { return m_wells.size(); }
    QString wellAt(int index) const;          // well id at section position

    void reorder(int from, int to);           // QList::move semantics (drag-reorder equivalent)
    bool isWellHighlighted(const QString &wellId) const;

    // --- log curves (compat single-curve API) --------------------------------
    // Replaces the well's whole track set with this ONE track. depths and
    // values are parallel sample arrays; NaN breaks the curve into series.
    // All columns share one depth axis — the global min/max depth across
    // every track and every horizon pick — so tracks stay depth-registered.
    void setWellCurves(const QString &wellId, const QVector<float> &depths,
                       const QVector<float> &values,
                       const QString &curveName = QString());
    void clearWellCurves();
    bool hasCurves() const { return !m_columns.isEmpty() && anyTracks(); }
    int curveItemCount(const QString &wellId) const; // rendered track pixmap items

    // Pull DEPT + `curveMnemonic` from a LAS file (io/LasParser), feed the
    // browser listing and add/replace that well's track for the mnemonic.
    // False on parse failure or an unknown mnemonic.
    bool loadWellLas(const QString &wellId, const QString &lasPath,
                     const QString &curveMnemonic);

    // --- multi-track ----------------------------------------------------------
    bool addWellTrack(const QString &wellId, const QString &mnemonic,
                      const QString &unit, const QVector<float> &depths,
                      const QVector<float> &values);
    bool removeWellTrack(const QString &wellId, const QString &mnemonic);
    QStringList wellTrackMnemonics(const QString &wellId) const;

    // --- LAS curve browser ------------------------------------------------------
    // Parses `lasPath` and populates the browser listing for `wellId`
    // (no tracks added). False on parse failure.
    bool setLasForWell(const QString &wellId, const QString &lasPath);
    CurveBrowser *curveBrowser() const { return m_browser; }

    // --- horizon correlation lines + flatten ------------------------------------
    HorizonMarkerSet *markers() const { return m_markers; }
    // Manifest-declared horizons (horizon=* declarations) seed the marker
    // set; SelectionContext's activeHorizon emphasizes the matching line.
    void setManifestHorizons(const QStringList &names);
    void setFlattenMarker(const QString &name);  // {} = off
    bool isFlattened() const;

    // --- 三视图联动（wave/mapping-pipeline 阶段C 本链路新增）--------------------
    // 滚动到某井某分层：水平居中该井柱；该井在此层位有 pick 时垂直也
    // 居中该 pick 线，否则居中柱体。未知井/层位仍记录目标（供状态断言
    // 与后续联动），视图不动。目标经 lastScroll* 暴露。
    void scrollToWellTop(const QString &wellId, const QString &horizon);
    QString lastScrollWell() const { return m_scrollWell; }
    QString lastScrollHorizon() const { return m_scrollHorizon; }

    // --- depth ruler --------------------------------------------------------------
    DepthRuler *ruler() const { return m_ruler; }

  signals:
    void wellClicked(const QString &wellId);
    void wellDoubleClicked(const QString &wellId);

  private:
    bool anyTracks() const;
    void rebuildScene();                        // selective: keeps chrome + marker items
    void relayoutMarkers();                     // rebuild marker lines for current geoms
    void computeDepthAxis();                    // display-space window incl. flatten offsets
    void applySelection(const QStringList &ids);

    qreal yForDepth(float displayDepth) const;  // display-space axis mapping
    float depthAtY(qreal y) const;

    SelectionContext *m_ctx = nullptr;
    QList<QPair<QString, QString>> m_wells;     // (id, name), section order
    QHash<QString, CorrelationWellColumn *> m_columns;      // id → column
    QHash<QString, QList<LasCurve>> m_lasByWell;            // id → last parsed LAS
    QList<QGraphicsPathItem *> m_columnItems;              // current scene columns
    QList<HorizonMarkerSet::ColumnGeom> m_lastGeoms;        // geoms matching m_columnItems
    QGraphicsRectItem *m_chrome = nullptr;     // marker parent; only while markers visible
    HorizonMarkerSet *m_markers = nullptr;
    DepthRuler *m_ruler = nullptr;             // view-painted, never a scene item
    CurveBrowser *m_browser = nullptr;
    QGraphicsView *m_view = nullptr;           // CorrelationScene/CorrelationView in .cpp
    QGraphicsScene *m_scene = nullptr;
    QLabel *m_emptyLabel = nullptr;
    float m_axisMin = 0.0f, m_axisMax = 100.0f; // shared display-space depth window
    QString m_scrollWell, m_scrollHorizon;      // 三视图联动最近一次滚动目标
    bool m_updatesEnabled = true;
    bool m_dirty = false;
};
