// 层：视图
#pragma once

#include <qgsabstractgeometry.h>
#include <qgsadvanceddigitizingdockwidget.h>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsmapcanvas.h>
#include <qgsproject.h>
#include <qgsvectorlayer.h>

namespace CaptureHelpers
{
// QgsMapToolAdvancedDigitizing requires a dock; fallback ownership is canvas-local.
inline QgsAdvancedDigitizingDockWidget *resolveCadDock(
    QgsMapCanvas *canvas, QgsAdvancedDigitizingDockWidget *given)
{
  return given ? given : new QgsAdvancedDigitizingDockWidget(canvas, canvas);
}

// Captured geometries use the active vector layer's CRS. Tools emit canvas CRS.
// The caller owns the geometry and keeps its own translated failure message.
inline bool transformToCanvas(QgsAbstractGeometry &geometry,
                              const QgsVectorLayer *layer, const QgsMapCanvas *canvas)
{
  if (!layer)
    return true;
  const QgsCoordinateReferenceSystem layerCrs = layer->crs();
  const QgsCoordinateReferenceSystem canvasCrs = canvas->mapSettings().destinationCrs();
  if (!layerCrs.isValid() || !canvasCrs.isValid() || layerCrs == canvasCrs)
    return true;
  try
  {
    const QgsCoordinateTransformContext context = layer->project()
        ? layer->project()->transformContext() : QgsProject::instance()->transformContext();
    geometry.transform(QgsCoordinateTransform(layerCrs, canvasCrs, context));
    return true;
  }
  catch (QgsCsException &)
  {
    return false;
  }
}
} // namespace CaptureHelpers
