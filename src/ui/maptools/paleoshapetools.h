#pragma once
#include <qgsmaptoolcapture.h>
#include <QString>

class QgsMapCanvas;
class QgsMapMouseEvent;
class QgsAdvancedDigitizingDockWidget;
class QKeyEvent;
class QgsPoint;
class QgsPointXY;

// ui/maptools/ — PaleoDrawPointTool: point constraint drawing tool
// (QgsMapToolCapture, CapturePoint).
// Single left-click commits constraintDrawn("Point (x y)") in canvas CRS.
// Esc or right-click cancels via drawAborted().
class PaleoDrawPointTool : public QgsMapToolCapture
{
  Q_OBJECT
  public:
    explicit PaleoDrawPointTool( QgsMapCanvas *canvas,
                                 QgsAdvancedDigitizingDockWidget *cadDock = nullptr );
    void activate() override;
    void deactivate() override;

  signals:
    void constraintDrawn( const QString &wkt );      // Point (x y) in canvas CRS
    void drawAborted();

  protected:
    void keyPressEvent( QKeyEvent *e ) override;
    void cadCanvasReleaseEvent( QgsMapMouseEvent *e ) override;
    void pointCaptured( const QgsPoint &point ) override;
};

// ui/maptools/ — PaleoDrawCircleTool: 24-vertex regular polygon approximation
// (CapturePolygon, StraightSegments).
// Two left-clicks = center + radius point -> "Polygon ((...))" 24-vertex approximation;
// right-click with 1 point planted uses cursor position as radius point;
// bare right-click or Esc aborts.
class PaleoDrawCircleTool : public QgsMapToolCapture
{
  Q_OBJECT
  public:
    explicit PaleoDrawCircleTool( QgsMapCanvas *canvas,
                                  QgsAdvancedDigitizingDockWidget *cadDock = nullptr );
    void activate() override;
    void deactivate() override;

  signals:
    void constraintDrawn( const QString &wkt );      // Polygon ((...)) in canvas CRS
    void drawAborted();

  protected:
    void keyPressEvent( QKeyEvent *e ) override;
    void cadCanvasReleaseEvent( QgsMapMouseEvent *e ) override;

  private:
    void emitCircle( const QgsPointXY *eventRadiusPoint = nullptr );
};

// ui/maptools/ — PaleoDrawEllipseTool: 36-vertex ellipse approximation
// (CapturePolygon, StraightSegments).
// Center + two axis endpoints (3 clicks) -> "Polygon ((...))" 36-vertex approximation;
// right-click with 2 points planted uses cursor position as axis 2 endpoint;
// bare right-click, right-click with 1 point, or Esc aborts.
class PaleoDrawEllipseTool : public QgsMapToolCapture
{
  Q_OBJECT
  public:
    explicit PaleoDrawEllipseTool( QgsMapCanvas *canvas,
                                   QgsAdvancedDigitizingDockWidget *cadDock = nullptr );
    void activate() override;
    void deactivate() override;

  signals:
    void constraintDrawn( const QString &wkt );      // Polygon ((...)) in canvas CRS
    void drawAborted();

  protected:
    void keyPressEvent( QKeyEvent *e ) override;
    void cadCanvasReleaseEvent( QgsMapMouseEvent *e ) override;

  private:
    void emitEllipse( const QgsPointXY *eventAxis2Point = nullptr );
};
