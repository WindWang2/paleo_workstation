#pragma once
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QVector>
#include <QWidget>

// ui/ — WellCorrelationPanel: 连井剖面 (well correlation section) scaffold.
//
// The constraint page's linked sub-panel: an ordered strip of well-log
// column placeholders tied to map selection through SelectionContext
// (§41.3). P1 scope = data model + scene scaffold only — real log-track
// rendering is deferred. Pure Qt Widgets: no Qgs* types appear here (§25),
// the panel emits intents (wellClicked/wellDoubleClicked) and follows
// selectionChanged broadcasts whose origin isn't "correlation" (its own
// echo guard — future producers broadcast under that origin).

class QGraphicsScene;
class QGraphicsPathItem;
class QGraphicsView;
class QLabel;
class SelectionContext;

class WellCorrelationPanel : public QWidget
{
  Q_OBJECT
  public:
    explicit WellCorrelationPanel(SelectionContext *ctx, QWidget *parent = nullptr);

    // (id, name) pairs in section order; re-layouts the scene.
    void setWells(const QList<QPair<QString, QString>> &wells);
    int wellCount() const { return m_wells.size(); }
    QString wellAt(int index) const;          // well id at section position

    void reorder(int from, int to);           // QList::move semantics (drag-reorder equivalent)
    bool isWellHighlighted(const QString &wellId) const;

    // --- log curves ---------------------------------------------------------
    // Real log rendering on top of the column scaffold: depths/values are
    // parallel sample arrays and NaN breaks the polyline into segments. All
    // columns share one depth axis — the global min/max depth across every
    // provided curve — so tracks stay depth-registered. Re-layouts the scene.
    void setWellCurves(const QString &wellId, const QVector<float> &depths,
                       const QVector<float> &values,
                       const QString &curveName = QString());
    void clearWellCurves();
    bool hasCurves() const { return !m_curves.isEmpty(); }
    int curveItemCount(const QString &wellId) const; // rendered curve path items

    // Pull DEPT + `curveMnemonic` from a LAS file (io/LasParser) and feed
    // setWellCurves(). False on parse failure or an unknown mnemonic.
    bool loadWellLas(const QString &wellId, const QString &lasPath,
                     const QString &curveMnemonic);

  signals:
    void wellClicked(const QString &wellId);
    void wellDoubleClicked(const QString &wellId);

  private:
    struct WellCurve
    {
      QVector<float> depths;                  // parallel to values
      QVector<float> values;
      QString name;                           // mnemonic — caption under the title
    };

    void rebuildScene();
    void applySelection(const QStringList &ids);
    void addCurveItems(QGraphicsPathItem *column, const WellCurve &curve,
                       float depthMin, float depthSpan);

    SelectionContext *m_ctx = nullptr;
    QList<QPair<QString, QString>> m_wells;   // (id, name), section order
    QHash<QString, WellCurve> m_curves;       // well id → log curve samples
    QGraphicsView *m_view = nullptr;
    QGraphicsScene *m_scene = nullptr;        // CorrelationScene, defined in .cpp
    QLabel *m_emptyLabel = nullptr;
};
