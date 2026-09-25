#pragma once
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingprovider.h>

// Spike-2: C++-only QgsProcessingAlgorithm wrapping GDALPolygonize.
// Raster -> polygons into a vector sink, proving the C++ processing path without Python.
class PaleoPolygonizeAlgorithm : public QgsProcessingAlgorithm
{
  public:
    QString name() const override { return QStringLiteral("paleo_polygonize"); }
    QString displayName() const override { return QStringLiteral("Paleo Polygonize (GDAL)"); }
    QString group() const override { return QStringLiteral("Paleo spikes"); }
    QString groupId() const override { return QStringLiteral("paleospikes"); }
    QString shortHelpString() const override { return QStringLiteral("Polygonize a raster band via GDALPolygonize — spike-2 C++ path."); }
    PaleoPolygonizeAlgorithm *createInstance() const override { return new PaleoPolygonizeAlgorithm(); }

    void initAlgorithm(const QVariantMap &configuration = QVariantMap()) override;
    QVariantMap processAlgorithm(const QVariantMap &parameters, QgsProcessingContext &context,
                                 QgsProcessingFeedback *feedback) override;
};

// QGIS 4.x: algorithms enter the registry only via a provider (no direct addAlgorithm).
class PaleoProcessingProvider : public QgsProcessingProvider
{
  public:
    QString id() const override { return QStringLiteral("paleospikes"); }
    QString name() const override { return QStringLiteral("Paleo spikes"); }
    void loadAlgorithms() override { addAlgorithm(new PaleoPolygonizeAlgorithm()); }
};
