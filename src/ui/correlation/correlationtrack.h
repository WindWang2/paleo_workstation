#pragma once
#include <QColor>
#include <QImage>
#include <QPair>
#include <QString>
#include <QVector>

// ui/correlation/ — CorrelationTrack: one log-curve track (vertical strip).
//
// Pure value/render component, no scene dependency: it owns one curve's
// samples plus presentation style and renders the curve body through
// QgsLineChartPlot — QGIS's plot engine draws the polyline, we never
// hand-build a QPainterPath. NaN/NULL samples split the samples into one
// QgsXyPlotSeries per contiguous finite run so gaps stay gaps (port of the
// series logic in correlationpanel.cpp::addCurveItems). Scene composition
// (pixmap item, captions, borders) belongs to CorrelationWellColumn.
//
// The depth range is always the SECTION-shared axis passed in at render
// time — a shallow curve inks only the top of its strip, keeping every
// track depth-registered with its neighbors.
class CorrelationTrack
{
  public:
    CorrelationTrack() = default;
    CorrelationTrack(const QString &mnemonic, const QString &unit = QString(),
                     const QVector<float> &depths = {}, const QVector<float> &values = {});

    // --- data ---------------------------------------------------------------
    void setData(const QVector<float> &depths, const QVector<float> &values);
    QVector<float> depths() const { return m_depths; }
    QVector<float> values() const { return m_values; }
    qsizetype sampleCount() const { return qMin(m_depths.size(), m_values.size()); }

    void setMnemonic(const QString &mnemonic) { m_mnemonic = mnemonic; }
    QString mnemonic() const { return m_mnemonic; }
    void setUnit(const QString &unit) { m_unit = unit; }
    QString unit() const { return m_unit; }

    // Fewer than 2 finite samples → nothing renderable.
    bool isEmpty() const;
    // Contiguous finite runs — the number of QgsXyPlotSeries render uses.
    int seriesCount() const;
    // Finite value min/max (for x-axis scaling); {0,0} when empty.
    QPair<float, float> valueRange() const;

    // --- presentation -------------------------------------------------------
    // Track stroke color. Curve colors are DATA symbols (like map-domain
    // palettes per DESIGN.md), not UI tokens; the default is text-muted.
    void setColor(const QColor &color) { m_color = color; }
    QColor color() const { return m_color; }
    // Preferred strip width in scene px (the column may override to fit).
    void setPreferredWidth(qreal px) { m_width = px; }
    qreal preferredWidth() const { return m_width; }
    // "GR · GAPI" style header caption text.
    QString caption() const;

    // --- rendering ----------------------------------------------------------
    // Renders the depth window [depthMin, depthMax] into a w×h image with
    // transparent background (column chrome draws around it). depthOffset
    // implements flatten-on-marker: a sample at real depth d is plotted at
    // display depth (d - depthOffset), so a flattened well's window shifts
    // with its marker pick (0 = unflattened). Returns a null image when
    // isEmpty(). Samples are min/max-decimated to ≤ 4×h points before the
    // series build — plot cost stays bounded on 50-well sections.
    QImage render(int w, int h, float depthMin, float depthMax,
                  float depthOffset = 0.0f) const;

  private:
    QString m_mnemonic;
    QString m_unit;
    QVector<float> m_depths;
    QVector<float> m_values;
    QColor m_color = QColor(QStringLiteral("#5D6E80")); // DESIGN.md text-muted
    qreal m_width = 90.0;
};
