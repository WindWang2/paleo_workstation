// 层：QGIS 封装
#include "mapfeaturepicker.h"
#include <QApplication>
#include <QCursor>
#include <QRect>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsmapmouseevent.h>
#include <qgsmaptoolselectutils.h>
#include <qgsrectangle.h>
#include <qgsgeometry.h>
#include <qgsmaptool.h>
#include <qgsrubberband.h>
#include <qgsmapcanvas.h>
#include <qgsvectorlayer.h>

namespace {
class PaleoSelectTool : public QgsMapTool
{
  public:
    explicit PaleoSelectTool( QgsMapCanvas *canvas )
      : QgsMapTool( canvas ), mPicker(canvas, this)
    {
      setToolName( QCoreApplication::translate( "PaleoEditingToolbar", "选择要素" ) );
      setCursor( QCursor( Qt::ArrowCursor ) );
    }

    Flags flags() const override { return ShowContextMenu; }

    ~PaleoSelectTool() override
    {
      // QGIS 4: QgsMapCanvasItem is no longer a QObject — delete, don't hide
      delete mRubberBand;
    }

    void activate() override
    {
      if ( !mRubberBand )
        mRubberBand = new QgsRubberBand( canvas(), Qgis::GeometryType::Polygon );
      QgsMapTool::activate();
    }

    void deactivate() override
    {
      mDragging = false;
      mPicker.clear();
      delete mRubberBand;
      mRubberBand = nullptr;
      QgsMapTool::deactivate();
    }

  protected:
    void canvasPressEvent( QgsMapMouseEvent *e ) override
    {
      if ( e->button() != Qt::LeftButton )
        return;
      mPicker.clear();
      mPressLayer = qobject_cast<QgsVectorLayer *>(canvas()->currentLayer());
      mPressPos = e->pixelPoint();
      mDragging = true;
    }

    void canvasMoveEvent( QgsMapMouseEvent *e ) override
    {
      if (!mDragging) {
        mPicker.hover(qobject_cast<QgsVectorLayer *>(canvas()->currentLayer()), e->pixelPoint());
        return;
      }
      QRect rect = QRect( mPressPos, e->pixelPoint() ).normalized();
      QgsMapToolSelectUtils::setRubberBand( canvas(), rect, mRubberBand );
    }

    void canvasReleaseEvent( QgsMapMouseEvent *e ) override
    {
      if ( e->button() != Qt::LeftButton || !mDragging )
        return;
      mDragging = false;
      QgsVectorLayer *vl = qobject_cast<QgsVectorLayer *>( canvas()->currentLayer() );
      if ( !vl )
      {
        emit messageEmitted( QCoreApplication::translate( "PaleoEditingToolbar",
                                   "选择操作需要当前图层为矢量图层" ),
                             Qgis::MessageLevel::Warning );
        mRubberBand->reset( Qgis::GeometryType::Polygon );
        return;
      }

      mRubberBand->reset(Qgis::GeometryType::Polygon);
      if (vl != mPressLayer) return;
      Qgis::SelectBehavior behavior = Qgis::SelectBehavior::SetSelection;
      if (e->modifiers() & Qt::ShiftModifier) behavior = Qgis::SelectBehavior::AddToSelection;
      else if (e->modifiers() & Qt::ControlModifier) behavior = Qgis::SelectBehavior::RemoveFromSelection;
      if ((mPressPos - e->pixelPoint()).manhattanLength() < QApplication::startDragDistance()) {
        const auto result = mPicker.choose(vl, e->pixelPoint(), false, [vl, behavior](qint64 id) {
          vl->selectByIds({id}, behavior);
        });
        if (result == MapFeaturePicker::Result::Empty && behavior == Qgis::SelectBehavior::SetSelection)
          vl->removeSelection();
        return;
      }
      QgsMapToolSelectUtils::setRubberBand(canvas(), QRect(mPressPos, e->pixelPoint()).normalized(), mRubberBand);
      QgsRectangle layerRect = mRubberBand->asGeometry().boundingBox(); // map CRS
      const QgsCoordinateReferenceSystem mapCrs = canvas()->mapSettings().destinationCrs();
      if ( mapCrs.isValid() && vl->crs().isValid() && mapCrs != vl->crs() )
      {
        try
        {
          layerRect = QgsCoordinateTransform( mapCrs, vl->crs(),
                                              canvas()->mapSettings().transformContext() )
                          .transformBoundingBox( layerRect );
        }
        catch ( QgsCsException & )
        {
          emit messageEmitted( QCoreApplication::translate( "PaleoEditingToolbar",
                                     "无法将选择矩形转换到图层坐标系" ),
                               Qgis::MessageLevel::Warning );
          mRubberBand->reset( Qgis::GeometryType::Polygon );
          return;
        }
      }

      vl->selectByRect( layerRect, behavior );
      mRubberBand->reset( Qgis::GeometryType::Polygon );
    }

  private:
    QgsRubberBand *mRubberBand = nullptr; // owned canvas item (drag box)
    MapFeaturePicker mPicker;
    QPointer<QgsVectorLayer> mPressLayer;
    QPoint mPressPos;
    bool mDragging = false;
};

}

QgsMapTool *MapFeaturePicker::createSelectionTool(QgsMapCanvas *canvas) { return new PaleoSelectTool(canvas); }
