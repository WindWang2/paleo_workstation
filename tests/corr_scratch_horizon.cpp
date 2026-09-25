#include <QtTest>
#include <QApplication>
#include <QCoreApplication>
#include <QGraphicsLineItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsSceneEvent>
#include <QGraphicsSimpleTextItem>
#include <QSignalSpy>

#include <cmath>
#include <limits>

#include "../src/ui/correlation/horizonmarkers.h"
#include "../src/ui/correlation/correlationwellcolumn.h"

// Subtask C scratch tests: HorizonMarkerSet model authority, flatten math,
// and the rebuild() scene composition (lines, labels, drag interaction).
// Axis used throughout: display depth d maps to scene y = 100 + (d - 1000).
class TestHorizonMarkers : public QObject
{
    Q_OBJECT

  private:
    static constexpr float kNaNf = std::numeric_limits<float>::quiet_NaN();

    static qreal yFor(float displayDepth) { return 100.0 + (double(displayDepth) - 1000.0); }
    static float depthFor(qreal y) { return float(1000.0 + (y - 100.0)); }

    static HorizonMarkerSet::ColumnGeom geom(const QString &wellId, qreal x)
    { return {wellId, QRectF(x, 0, 50, 400)}; }

    static QList<QGraphicsItem *> markerItems(const QGraphicsScene *s)
    {
        QList<QGraphicsItem *> out;
        for (QGraphicsItem *it : s->items())
            if (it->data(CorrelationItemRoles::HorizonMarker).isValid())
                out << it;
        return out;
    }

    static QList<QGraphicsLineItem *> linesOf(const QGraphicsScene *s, const QString &marker)
    {
        QList<QGraphicsLineItem *> out;
        for (QGraphicsItem *it : s->items())
            if (it->data(CorrelationItemRoles::HorizonMarker).toString() == marker)
                if (auto *l = qgraphicsitem_cast<QGraphicsLineItem *>(it))
                    out << l;
        return out;
    }

    static QList<QGraphicsSimpleTextItem *> labelsOf(const QGraphicsScene *s, const QString &marker)
    {
        QList<QGraphicsSimpleTextItem *> out;
        for (QGraphicsItem *it : s->items())
            if (it->data(CorrelationItemRoles::HorizonMarker).toString() == marker)
                if (auto *t = qgraphicsitem_cast<QGraphicsSimpleTextItem *>(it))
                    out << t;
        return out;
    }

    // The marker line item for (marker, well) — identified purely by the
    // frozen data roles, never by type knowledge of the private subclass.
    static QGraphicsLineItem *lineFor(const QGraphicsScene *s, const QString &marker,
                                      const QString &wellId)
    {
        for (QGraphicsItem *it : s->items())
            if (it->data(CorrelationItemRoles::HorizonMarker).toString() == marker
                && it->data(CorrelationItemRoles::WellId).toString() == wellId)
                if (auto *l = qgraphicsitem_cast<QGraphicsLineItem *>(it))
                    return l;
        return nullptr;
    }

    // Scene-level delivery (full path incl. grab bookkeeping).
    static void sceneMouseEvent(QGraphicsScene *scene, QEvent::Type type,
                                const QPointF &scenePos, Qt::MouseButton button,
                                Qt::MouseButtons buttons)
    {
        QGraphicsSceneMouseEvent e(type);
        e.setPos(scenePos);
        e.setScenePos(scenePos);
        e.setScreenPos(QPoint(0, 0));
        e.setButton(button);
        e.setButtons(buttons);
        e.setModifiers(Qt::NoModifier);
        QCoreApplication::sendEvent(scene, &e);
    }

    // Direct item delivery (bypasses scene grab logic).
    static void itemMouseEvent(QGraphicsScene *scene, QGraphicsItem *item, QEvent::Type type,
                               const QPointF &scenePos, Qt::MouseButton button,
                               Qt::MouseButtons buttons)
    {
        QGraphicsSceneMouseEvent e(type);
        e.setPos(item->mapFromScene(scenePos));
        e.setScenePos(scenePos);
        e.setScreenPos(QPoint(0, 0));
        e.setButton(button);
        e.setButtons(buttons);
        e.setModifiers(Qt::NoModifier);
        scene->sendEvent(item, &e);
    }

  private slots:
    // --- model ---------------------------------------------------------------

    void manifestHorizonsAreAuthoritative()
    {
        HorizonMarkerSet set;
        QVERIFY(set.addMarker(QStringLiteral("A")));
        set.setWellDepth(QStringLiteral("A"), QStringLiteral("W1"), 1050.f);
        set.setMarkerColor(QStringLiteral("A"), QColor(QStringLiteral("#123456")));

        // Declared set is authoritative: adds missing (default color),
        // keeps existing picks/colors, order follows the declaration.
        set.setManifestHorizons({QStringLiteral("B"), QStringLiteral("A")});
        QCOMPARE(set.markerNames(),
                 QStringList({QStringLiteral("B"), QStringLiteral("A")}));
        QCOMPARE(set.wellDepth(QStringLiteral("A"), QStringLiteral("W1")), 1050.f);
        QCOMPARE(set.markerColor(QStringLiteral("A")), QColor(QStringLiteral("#123456")));
        QCOMPARE(set.markerColor(QStringLiteral("B")), QColor(QStringLiteral("#F29900")));
        QCOMPARE(set.wellsPicked(QStringLiteral("B")).size(), 0);

        // Undeclared markers are REMOVED — picks go with them, and a
        // flatten on a removed marker turns off.
        set.setFlattenMarker(QStringLiteral("A"));
        QVERIFY(set.isFlattened());
        set.setManifestHorizons({QStringLiteral("B")});
        QVERIFY(!set.hasMarker(QStringLiteral("A")));
        QVERIFY(std::isnan(set.wellDepth(QStringLiteral("A"), QStringLiteral("W1"))));
        QVERIFY(!set.isFlattened());
    }

    void markerColorApi()
    {
        HorizonMarkerSet set;
        QVERIFY(!set.addMarker(QString()));          // empty name rejected
        QVERIFY(set.addMarker(QStringLiteral("M")));
        QVERIFY(!set.addMarker(QStringLiteral("M"))); // duplicate rejected
        QVERIFY(set.hasMarker(QStringLiteral("M")));
        QCOMPARE(set.markerColor(QStringLiteral("M")), QColor(QStringLiteral("#F29900")));
        set.setMarkerColor(QStringLiteral("M"), QColor(QStringLiteral("#00AA00")));
        QCOMPARE(set.markerColor(QStringLiteral("M")), QColor(QStringLiteral("#00AA00")));
        QVERIFY(!set.markerColor(QStringLiteral("NOPE")).isValid());

        QVERIFY(set.removeMarker(QStringLiteral("M")));
        QVERIFY(!set.removeMarker(QStringLiteral("M")));
        QVERIFY(!set.hasMarker(QStringLiteral("M")));
        QVERIFY(set.markerNames().isEmpty());
    }

    void flattenTransformMath()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W2"), 1062.5f);
        // W3 has no pick.

        // Flatten off: identity.
        QCOMPARE(set.displayOffset(QStringLiteral("W1")), 0.f);
        QCOMPARE(set.displayDepth(QStringLiteral("W1"), 1070.f), 1070.f);
        QVERIFY(!set.isFlattened());

        set.setFlattenMarker(QStringLiteral("M"));
        QVERIFY(set.isFlattened());
        QCOMPARE(set.flattenMarker(), QStringLiteral("M"));
        // Every picked well's M lands at display 0; offsets are the picks.
        QCOMPARE(set.displayDepth(QStringLiteral("W1"), 1050.f), 0.f);
        QCOMPARE(set.displayDepth(QStringLiteral("W2"), 1062.5f), 0.f);
        QCOMPARE(set.displayDepth(QStringLiteral("W1"), 1100.f), 50.f);
        QCOMPARE(set.displayOffset(QStringLiteral("W1")), 1050.f);
        QCOMPARE(set.displayOffset(QStringLiteral("W2")), 1062.5f);
        // Unpicked well keeps the identity mapping.
        QCOMPARE(set.displayOffset(QStringLiteral("W3")), 0.f);
        QCOMPARE(set.displayDepth(QStringLiteral("W3"), 1234.f), 1234.f);

        set.setFlattenMarker(QStringLiteral("NOPE")); // unknown → off
        QVERIFY(!set.isFlattened());
        set.setFlattenMarker(QStringLiteral("M"));
        set.setFlattenMarker(QString());              // empty → off
        QVERIFY(!set.isFlattened());
    }

    void wellDepthSignal()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        QSignalSpy spy(&set, &HorizonMarkerSet::markerDepthChanged);

        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1010.f);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), QStringLiteral("M"));
        QCOMPARE(spy.first().at(1).toString(), QStringLiteral("W1"));
        QCOMPARE(spy.first().at(2).toFloat(), 1010.f);

        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1020.f);
        QCOMPARE(spy.count(), 2);
        set.setWellDepth(QStringLiteral("NOPE"), QStringLiteral("W1"), 1.f); // unknown: silent
        QCOMPARE(spy.count(), 2);

        QCOMPARE(set.wellDepth(QStringLiteral("M"), QStringLiteral("W1")), 1020.f);
        QVERIFY(std::isnan(set.wellDepth(QStringLiteral("M"), QStringLiteral("W9"))));
        QCOMPARE(set.wellsPicked(QStringLiteral("M")),
                 QStringList{QStringLiteral("W1")});
    }

    // --- scene composition -----------------------------------------------------

    void rebuildLineGeometry()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W2"), 1060.f);

        QGraphicsScene scene;
        // W3 exists as a column but carries no M pick — no line for it.
        const QList<HorizonMarkerSet::ColumnGeom> geoms{
            geom(QStringLiteral("W1"), 0), geom(QStringLiteral("W2"), 100),
            geom(QStringLiteral("W3"), 200)};

        // Flattened: both picked wells draw M at display depth 0 → the
        // two column segments share ONE y (yFor(0) = -900 on this axis)
        // — the one-horizontal-line proof.
        set.setFlattenMarker(QStringLiteral("M"));
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, true);
        auto lines = linesOf(&scene, QStringLiteral("M"));
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.at(0)->line().y1(), yFor(0.f)); // = -900
        QCOMPARE(lines.at(0)->line().y2(), yFor(0.f));
        QCOMPARE(lines.at(1)->line().y1(), yFor(0.f));
        QCOMPARE(lines.at(1)->line().y2(), yFor(0.f));
        QVERIFY(lines.at(0)->line().y1() == lines.at(1)->line().y1());
        // Lines span their own column rects; frozen click-resolution roles.
        auto *w1line = lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"));
        auto *w2line = lineFor(&scene, QStringLiteral("M"), QStringLiteral("W2"));
        QVERIFY(w1line && w2line);
        QCOMPARE(w1line->line().x1(), 0.0);
        QCOMPARE(w1line->line().x2(), 50.0);
        QCOMPARE(w2line->line().x1(), 100.0);
        QCOMPARE(w2line->line().x2(), 150.0);
        QCOMPARE(w1line->data(CorrelationItemRoles::HorizonMarker).toString(),
                 QStringLiteral("M"));
        QCOMPARE(w1line->data(CorrelationItemRoles::WellId).toString(),
                 QStringLiteral("W1"));

        // Unflattened: same picks, different ys — depth-registered per well.
        set.setFlattenMarker(QString());
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, false);
        lines = linesOf(&scene, QStringLiteral("M"));
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"))->line().y1(),
                 150.0); // yFor(1050)
        QCOMPARE(lineFor(&scene, QStringLiteral("M"), QStringLiteral("W2"))->line().y1(),
                 160.0); // yFor(1060)
        QVERIFY(lines.at(0)->line().y1() != lines.at(1)->line().y1());
        // Geometry agrees with lineDepthAt through the axis mapping.
        QCOMPARE(lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"))->line().y1(),
                 (double)yFor(set.lineDepthAt(QStringLiteral("M"), QStringLiteral("W1"))));
        QCOMPARE(lineFor(&scene, QStringLiteral("M"), QStringLiteral("W2"))->line().y1(),
                 (double)yFor(set.lineDepthAt(QStringLiteral("M"), QStringLiteral("W2"))));
        QCOMPARE(lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"))->pen().widthF(), 1.5);
        QCOMPARE(lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"))->pen().color(),
                 QColor(QStringLiteral("#F29900")));
        // 2 lines + 1 label; nothing accumulates across rebuilds.
        QCOMPARE(markerItems(&scene).size(), 3);

        // Parented variant: every marker item hangs under `parent`, so the
        // scene's top level stays exactly the panel's own items.
        auto *backdrop = scene.addRect(QRectF(-10, -10, 400, 500), QPen(Qt::NoPen));
        set.rebuild(&scene, backdrop, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, true, QStringLiteral("M"));
        QCOMPARE(markerItems(&scene).size(), 3); // old ones reaped by role
        int topLevel = 0;
        for (QGraphicsItem *it : scene.items())
            if (!it->parentItem())
                ++topLevel;
        QCOMPARE(topLevel, 1); // the backdrop only
        for (QGraphicsItem *it : markerItems(&scene))
            QCOMPARE(it->parentItem(), backdrop);
        // Active-horizon emphasis landed here too (see dedicated test).
        QCOMPARE(lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"))->pen().widthF(), 2.5);
    }

    void lineDepthAtSpaces()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.addMarker(QStringLiteral("N"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);
        set.setWellDepth(QStringLiteral("N"), QStringLiteral("W1"), 1100.f);
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W2"), 1060.f);

        QCOMPARE(set.lineDepthAt(QStringLiteral("M"), QStringLiteral("W1")), 1050.f);
        QCOMPARE(set.lineDepthAt(QStringLiteral("N"), QStringLiteral("W1")), 1100.f);
        QVERIFY(std::isnan(set.lineDepthAt(QStringLiteral("N"), QStringLiteral("W2"))));
        QVERIFY(std::isnan(set.lineDepthAt(QStringLiteral("NOPE"), QStringLiteral("W1"))));
        QVERIFY(std::isnan(set.lineDepthAt(QStringLiteral("M"), QStringLiteral("W9"))));

        set.setFlattenMarker(QStringLiteral("M"));
        QCOMPARE(set.lineDepthAt(QStringLiteral("M"), QStringLiteral("W1")), 0.f);
        QCOMPARE(set.lineDepthAt(QStringLiteral("M"), QStringLiteral("W2")), 0.f);
        QCOMPARE(set.lineDepthAt(QStringLiteral("N"), QStringLiteral("W1")), 50.f);
    }

    void activeHorizonPenAndZ()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.addMarker(QStringLiteral("N"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);
        set.setWellDepth(QStringLiteral("N"), QStringLiteral("W1"), 1100.f);

        QGraphicsScene scene;
        const QList<HorizonMarkerSet::ColumnGeom> geoms{geom(QStringLiteral("W1"), 0)};
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, false, QStringLiteral("M"));

        auto *active = lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"));
        auto *normal = lineFor(&scene, QStringLiteral("N"), QStringLiteral("W1"));
        QVERIFY(active && normal);
        QCOMPARE(active->pen().widthF(), 2.5); // active: thicker…
        QCOMPARE(active->zValue(), 1.0);      // …and one Z level up
        QCOMPARE(normal->pen().widthF(), 1.5);
        QCOMPARE(normal->zValue(), 0.0);
        // Labels read above every line.
        QCOMPARE(labelsOf(&scene, QStringLiteral("M")).first()->zValue(), 2.0);
    }

    void labelPerMarkerAtLeftmostPick()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W3"), 1100.f); // W2 unpicked
        set.addMarker(QStringLiteral("N"), QColor(QStringLiteral("#123456")));
        set.setWellDepth(QStringLiteral("N"), QStringLiteral("W3"), 1150.f);

        QGraphicsScene scene;
        const QList<HorizonMarkerSet::ColumnGeom> geoms{
            geom(QStringLiteral("W1"), 0), geom(QStringLiteral("W2"), 60),
            geom(QStringLiteral("W3"), 120)};
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, false);

        // M's line ys: W1 → 150, W3 → 200; N's: W3 → 250.
        const auto mLabels = labelsOf(&scene, QStringLiteral("M"));
        QCOMPARE(mLabels.size(), 1); // one label per marker, not per column
        auto *lbl = mLabels.first();
        QCOMPARE(lbl->text(), QStringLiteral("M"));
        QCOMPARE(lbl->brush().color(), QColor(QStringLiteral("#F29900")));
        QCOMPARE(lbl->font().pointSizeF(), 8.0);
        QCOMPARE(lbl->pos().x(), 0.0); // anchored at the LEFTMOST picked column
        QCOMPARE(lbl->pos().y() + lbl->boundingRect().height(), 148.0); // 2px above y=150
        QVERIFY(lbl->acceptedMouseButtons() == Qt::NoButton); // mouse-transparent

        const auto nLabels = labelsOf(&scene, QStringLiteral("N"));
        QCOMPARE(nLabels.size(), 1);
        QCOMPARE(nLabels.first()->pos().x(), 120.0); // W3 is N's only picked column
        QCOMPARE(nLabels.first()->pos().y() + nLabels.first()->boundingRect().height(),
                 248.0); // 2px above y=250
        QCOMPARE(nLabels.first()->brush().color(), QColor(QStringLiteral("#123456")));

        QCOMPARE(markerItems(&scene).size(), 5); // 3 lines + 2 labels
    }

    void nonEditableLinesIgnoreMouse()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);

        QGraphicsScene scene;
        const QList<HorizonMarkerSet::ColumnGeom> geoms{geom(QStringLiteral("W1"), 0)};
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, false);

        auto *line = lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"));
        QVERIFY(line);
        QVERIFY(line->acceptedMouseButtons() == Qt::NoButton); // never swallows clicks
        QVERIFY(!line->acceptHoverEvents());

        // Scene-level press right on the line falls through to the column
        // beneath: no grab, no pick change, no signal.
        QSignalSpy spy(&set, &HorizonMarkerSet::markerDepthChanged);
        sceneMouseEvent(&scene, QEvent::GraphicsSceneMousePress, QPointF(25, 150),
                        Qt::LeftButton, Qt::LeftButton);
        QVERIFY(scene.mouseGrabberItem() == nullptr);
        sceneMouseEvent(&scene, QEvent::GraphicsSceneMouseRelease, QPointF(25, 150),
                        Qt::LeftButton, Qt::NoButton);
        QCOMPARE(spy.count(), 0);
        QCOMPARE(set.wellDepth(QStringLiteral("M"), QStringLiteral("W1")), 1050.f);
    }

    void hoverCursorFeedback()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);

        QGraphicsScene scene;
        const QList<HorizonMarkerSet::ColumnGeom> geoms{geom(QStringLiteral("W1"), 0)};
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, true);

        auto *line = lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"));
        QVERIFY(line->acceptHoverEvents());

        QGraphicsSceneHoverEvent enter(QEvent::GraphicsSceneHoverEnter);
        scene.sendEvent(line, &enter);
        QCOMPARE(line->cursor().shape(), Qt::SizeVerCursor); // vertical affordance

        QGraphicsSceneHoverEvent leave(QEvent::GraphicsSceneHoverLeave);
        scene.sendEvent(line, &leave);
        QCOMPARE(line->cursor().shape(), Qt::ArrowCursor);
    }

    // --- drag interaction --------------------------------------------------------

    void dragDirectCommitsPick()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);

        QGraphicsScene scene;
        const QList<HorizonMarkerSet::ColumnGeom> geoms{geom(QStringLiteral("W1"), 0)};
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, true);

        auto *line = lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"));
        QSignalSpy spy(&set, &HorizonMarkerSet::markerDepthChanged);

        // Press 2px inside the grab band: must NOT snap the line.
        itemMouseEvent(&scene, line, QEvent::GraphicsSceneMousePress, QPointF(25, 152),
                       Qt::LeftButton, Qt::LeftButton);
        QCOMPARE(line->line().y1(), 150.0);

        itemMouseEvent(&scene, line, QEvent::GraphicsSceneMouseMove, QPointF(25, 192),
                       Qt::NoButton, Qt::LeftButton);
        QCOMPARE(line->line().y1(), 190.0); // line-anchored, not cursor-snapped
        QCOMPARE(line->line().y2(), 190.0);
        QCOMPARE(line->line().x1(), 0.0);   // x is hard-locked to the column span
        QCOMPARE(line->line().x2(), 50.0);

        itemMouseEvent(&scene, line, QEvent::GraphicsSceneMouseRelease, QPointF(25, 192),
                       Qt::LeftButton, Qt::NoButton);

        // The drag commit is deferred off the item's event stack.
        QTest::qWait(30);
        QCOMPARE(set.wellDepth(QStringLiteral("M"), QStringLiteral("W1")), 1090.f);
        QVERIFY(spy.count() >= 1);
        QCOMPARE(spy.last().at(0).toString(), QStringLiteral("M"));
        QCOMPARE(spy.last().at(1).toString(), QStringLiteral("W1"));
        QCOMPARE(spy.last().at(2).toFloat(), 1090.f);
        QCOMPARE(set.lineDepthAt(QStringLiteral("M"), QStringLiteral("W1")), 1090.f);
    }

    void dragThroughSceneDelivery()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);

        QGraphicsScene scene;
        const QList<HorizonMarkerSet::ColumnGeom> geoms{geom(QStringLiteral("W1"), 0)};
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, true);

        auto *line = lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"));
        sceneMouseEvent(&scene, QEvent::GraphicsSceneMousePress, QPointF(25, 150),
                        Qt::LeftButton, Qt::LeftButton);
        QCOMPARE(scene.mouseGrabberItem(), line); // real grab through the scene

        sceneMouseEvent(&scene, QEvent::GraphicsSceneMouseMove, QPointF(25, 190),
                        Qt::NoButton, Qt::LeftButton);
        QCOMPARE(line->line().y1(), 190.0);

        sceneMouseEvent(&scene, QEvent::GraphicsSceneMouseRelease, QPointF(25, 190),
                        Qt::LeftButton, Qt::NoButton);
        QVERIFY(scene.mouseGrabberItem() == nullptr);

        QTest::qWait(30);
        QCOMPARE(set.wellDepth(QStringLiteral("M"), QStringLiteral("W1")), 1090.f);
    }

    void rebuildPreservesDraggedLine()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W2"), 1060.f);

        QGraphicsScene scene;
        const QList<HorizonMarkerSet::ColumnGeom> geoms{
            geom(QStringLiteral("W1"), 0), geom(QStringLiteral("W2"), 100)};
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, true);

        auto *dragged = lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"));
        QVERIFY(dragged);
        // Sentinel to tell preserved items from fresh ones — pointer
        // identity alone is unreliable (a fresh item can reuse the heap
        // block of a deleted one).
        constexpr int kSentinel = 999;
        dragged->setData(kSentinel, QStringLiteral("old"));

        itemMouseEvent(&scene, dragged, QEvent::GraphicsSceneMousePress, QPointF(25, 150),
                       Qt::LeftButton, Qt::LeftButton);
        itemMouseEvent(&scene, dragged, QEvent::GraphicsSceneMouseMove, QPointF(25, 190),
                       Qt::NoButton, Qt::LeftButton);
        dragged->grabMouse();
        QCOMPARE(scene.mouseGrabberItem(), dragged);
        QTest::qWait(30); // deferred commit lands: W1 pick → 1090

        // A panel-style full rebuild while the drag is still held must not
        // kill the grab: the grabbed line is updated in place.
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, true);
        QCOMPARE(scene.mouseGrabberItem(), dragged);  // grab survived
        QCOMPARE(linesOf(&scene, QStringLiteral("M")).size(), 2);
        QCOMPARE(lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1")),
                 dragged);                            // same item, kept…
        QCOMPARE(dragged->line().y1(), 190.0);       // …re-positioned from the pick
        QCOMPARE(dragged->line().x1(), 0.0);
        QVERIFY(dragged->data(kSentinel).isValid()); // …not silently recreated
        auto *sibling2 = lineFor(&scene, QStringLiteral("M"), QStringLiteral("W2"));
        QVERIFY(!sibling2->data(kSentinel).isValid()); // untouched lines WERE recreated
        QCOMPARE(sibling2->line().y1(), 160.0);

        dragged->ungrabMouse();
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, true);
        QVERIFY(scene.mouseGrabberItem() == nullptr);
        // No drag in flight: everything fresh — the old (sentinel-carrying)
        // item is gone even if its address was recycled.
        QVERIFY(!lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"))
                     ->data(kSentinel).isValid());
    }

    void draggingFlattenMarkerShiftsDataNotLine()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W2"), 1060.f);
        set.setFlattenMarker(QStringLiteral("M"));

        QGraphicsScene scene;
        const QList<HorizonMarkerSet::ColumnGeom> geoms{
            geom(QStringLiteral("W1"), 0), geom(QStringLiteral("W2"), 100)};
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, true);

        // Flattened M is pinned at display 0 in every picked well
        // (yFor(0) = -900 on this axis).
        auto *w1 = lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"));
        auto *w2 = lineFor(&scene, QStringLiteral("M"), QStringLiteral("W2"));
        QCOMPARE(w1->line().y1(), yFor(0.f));
        QCOMPARE(w2->line().y1(), yFor(0.f));

        // Drag W2's M line 40 depth units down (press 2px inside the band).
        const qreal pinned = yFor(0.f);
        itemMouseEvent(&scene, w2, QEvent::GraphicsSceneMousePress, QPointF(125, pinned + 2),
                       Qt::LeftButton, Qt::LeftButton);
        itemMouseEvent(&scene, w2, QEvent::GraphicsSceneMouseMove, QPointF(125, pinned + 42),
                       Qt::NoButton, Qt::LeftButton);
        itemMouseEvent(&scene, w2, QEvent::GraphicsSceneMouseRelease, QPointF(125, pinned + 42),
                       Qt::LeftButton, Qt::NoButton);
        QTest::qWait(30);

        // The pick moved by exactly the dragged delta (1060 + 40)…
        QCOMPARE(set.wellDepth(QStringLiteral("M"), QStringLiteral("W2")), 1100.f);
        QCOMPARE(set.displayOffset(QStringLiteral("W2")), 1100.f);
        // …and the flatten line itself stays pinned at display depth 0 —
        // the well's DATA shifts, not the datum.
        QCOMPARE(set.lineDepthAt(QStringLiteral("M"), QStringLiteral("W2")), 0.f);
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, true);
        QCOMPARE(lineFor(&scene, QStringLiteral("M"), QStringLiteral("W2"))->line().y1(), yFor(0.f));
        QCOMPARE(lineFor(&scene, QStringLiteral("M"), QStringLiteral("W1"))->line().y1(), yFor(0.f));
    }

    void rebuildDropsItemsWithoutPicks()
    {
        HorizonMarkerSet set;
        set.addMarker(QStringLiteral("M"));
        set.setWellDepth(QStringLiteral("M"), QStringLiteral("W1"), 1050.f);
        set.addMarker(QStringLiteral("N"));
        set.setWellDepth(QStringLiteral("N"), QStringLiteral("W1"), 1100.f);

        QGraphicsScene scene;
        const QList<HorizonMarkerSet::ColumnGeom> geoms{geom(QStringLiteral("W1"), 0)};
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, false);
        QCOMPARE(markerItems(&scene).size(), 4); // 2 lines + 2 labels

        // Marker removed from the model → its items vanish on rebuild.
        set.removeMarker(QStringLiteral("N"));
        set.rebuild(&scene, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, false);
        QCOMPARE(markerItems(&scene).size(), 2);
        QVERIFY(labelsOf(&scene, QStringLiteral("N")).isEmpty());

        // Empty geometry (e.g. no wells) → everything reaped.
        set.rebuild(&scene, nullptr, {}, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, false);
        QCOMPARE(markerItems(&scene).size(), 0);

        // Null scene is a safe no-op.
        set.rebuild(nullptr, nullptr, geoms, &TestHorizonMarkers::yFor,
                    &TestHorizonMarkers::depthFor, false);
    }
};

int main(int argc, char *argv[])
{
    if (qgetenv("QT_QPA_PLATFORM").isEmpty())
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    TestHorizonMarkers t;
    return QTest::qExec(&t, argc, argv);
}

#include "corr_scratch_horizon.moc"
