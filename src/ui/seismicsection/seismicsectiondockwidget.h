#pragma once

#include <QDockWidget>
#include <QLabel>
#include <QToolButton>
#include <QComboBox>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QProgressBar>
#include <memory>

#include "ui/seismicsection/seismicsectioncanvas.h"

namespace seismic {

class SeismicSectionDockWidget : public QDockWidget {
    Q_OBJECT

public:
    explicit SeismicSectionDockWidget(QWidget *parent = nullptr);
    explicit SeismicSectionDockWidget(const QString &title, QWidget *parent = nullptr);
    ~SeismicSectionDockWidget() override = default;

    SeismicSectionCanvas *canvas() const { return m_canvas; }

    // Direct data feed
    void setSectionData(const SgySliceImage &image,
                        float sampleIntervalMs = 2.0f,
                        double startSampleMs = 0.0f,
                        const std::vector<float> &columnDistancesM = {},
                        const std::vector<glm::dvec2> &mapCoords = {});

    void setWells(const std::vector<SectionWellInfo> &wells);
    void setTimeDepthModel(const TimeDepthModel &model);
    void setLineTitle(const QString &title);

    // Asynchronous background extraction of arbitrary or well section path
    void extractSectionFromVolumeAsync(
        std::shared_ptr<const SgyVolume> volume,
        const std::vector<glm::ivec2> &pathPoints,
        const QString &lineTitle = QString(),
        const std::vector<glm::dvec2> &mapPolyline = {},
        const std::vector<SectionWellInfo> &candidateWells = {});

signals:
    void sectionExtractionFinished(bool success, const QString &message);
    void pointClickedOnMap(double x, double y);

private slots:
    void onZoomChanged(double zoom);
    void onTraceHovered(int traceIndex, double twtMs, double depthM, float amplitude, double mapX, double mapY);
    void onExportSnapshot();

private:
    void setupUi();

    SeismicSectionCanvas *m_canvas = nullptr;

    // Toolbar widgets
    QLabel *m_lblTitle = nullptr;
    QToolButton *m_btnZoomIn = nullptr;
    QToolButton *m_btnZoomOut = nullptr;
    QToolButton *m_btnFit = nullptr;
    QToolButton *m_btnReset = nullptr;
    QToolButton *m_btnUnitToggle = nullptr;
    QComboBox *m_cboColorMap = nullptr;
    QSlider *m_sliderGain = nullptr;
    QDoubleSpinBox *m_spinGain = nullptr;
    QToolButton *m_btnWellOptions = nullptr;
    QToolButton *m_btnExport = nullptr;

    // Status bar widgets
    QLabel *m_lblCoordinates = nullptr;
    QProgressBar *m_progressBar = nullptr;
};

} // namespace seismic
