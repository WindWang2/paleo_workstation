#pragma once
#include <QGraphicsItem>
#include <QList>
#include <QString>

class QPainter;
class QStyleOptionGraphicsItem;
class QWidget;

// ui/correlation/ — DepthRuler: the section's left-hand depth scale.
//
// A QGraphicsItem (parented under the panel's chrome layer) drawing major
// and minor ticks against the depth window the view currently shows:
// when the panel zooms, the window narrows and the tick step re-nices to
// 1/2/5 × 10^k so labels stay readable instead of crowding. Units are
// switchable m/ft (LAS depths are meters; ft is a display conversion).
// Grid lines extend across the section behind the columns when enabled.
//
// Readability guardrail: with the default niceStep target of 6, the 1/2/5
// thresholds guarantee span/step < 9 for ANY finite range, so majorDepths()
// never exceeds 9 labeled ticks (asserted in tests).
//
// Tick math is pure and exposed for tests: readable scales are asserted
// on majorDepths()/minorDepths() without rasterizing.
class DepthRuler : public QGraphicsItem
{
  public:
    enum class Unit
    {
      Meters,
      Feet
    };

    DepthRuler();

    // Depth window currently displayed (source units = meters). Ticks are
    // computed in DISPLAY units so labels read naturally in ft mode.
    void setRange(float depthMin, float depthMax);
    float rangeMin() const { return m_min; }
    float rangeMax() const { return m_max; }
    void setUnit(Unit unit);
    Unit unit() const { return m_unit; }
    void setGridVisible(bool on)
    {
      if (m_grid == on) return;
      prepareGeometryChange();   // grid on/off changes the bounding width
      m_grid = on;
      update();
    }
    bool gridVisible() const { return m_grid; }
    // Length (scene px) the major-tick grid lines extend right of the label
    // column, into the section area. 0 = no grid reach.
    void setGridWidth(qreal px);
    qreal gridWidth() const { return m_gridWidth; }
    void setLabelWidth(qreal px)
    {
      const qreal w = qMax(px, 8.0);
      if (w == m_labelWidth) return;
      prepareGeometryChange();   // label column width anchors the ticks
      m_labelWidth = w;
      update();
    }

    // Vertical extent the ruler maps the range onto (scene px).
    void setHeight(qreal px);
    qreal height() const { return m_height; }

    // --- tick math (pure, test-friendly) --------------------------------------
    // Nice step for covering `span` (display units) with ≲ target majors:
    // 1/2/5 × 10^k. Thresholds are tuned so that with the default target
    // of 6 the step always covers the span in < 9 intervals (≤ 9 ticks).
    // Exposed static for direct testing.
    static double niceStep(double span, int targetMajors = 6);
    double majorStep() const;                  // display units
    QList<double> majorDepths() const;         // ascending, in range
    QList<double> minorDepths() const;         // 5 minors per major
    // "1250" — no unit suffix; mono font (DESIGN.md tnum) is applied in paint.
    QString labelText(double depth) const;
    // Scene y for a depth in the current window (top = rangeMin).
    qreal yForDepth(float depth) const;

    // --- QGraphicsItem ---------------------------------------------------------
    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
               QWidget *widget = nullptr) override;

  private:
    float m_min = 0.0f;
    float m_max = 100.0f;
    Unit m_unit = Unit::Meters;
    bool m_grid = true;
    qreal m_height = 380.0;
    qreal m_labelWidth = 46.0;
    qreal m_gridWidth = 0.0;
    mutable double m_majorStep = 0.0;   // cached, recomputed on range change
    mutable bool m_stepDirty = true;

    // meters→display-units factor for the current unit (1 / 3.280839895).
    double displayFactor() const;
};
