#include "paleodecorations.h"

#include <cmath>

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPen>
#include <QPolygonF>

#include <qgsmapcanvas.h>
#include <qgsmapsettings.h>
#include <qgsmaptopixel.h>
#include <qgsrendercontext.h>
#include <qgsrectangle.h>
#include <qgsunittypes.h>

namespace
{
  // Largest "nice" (1/2/5 x 10^n) value <= value, for round-number
  // scale-bar lengths and grid intervals.
  double niceDistance( double value )
  {
    if ( !( value > 0 ) || !std::isfinite( value ) )
      return 0;
    const double mag = std::pow( 10.0, std::floor( std::log10( value ) ) );
    const double n = value / mag;
    const double nice = ( n >= 5.0 ) ? 5.0 : ( n >= 2.0 ? 2.0 : 1.0 );
    return nice * mag;
  }

  int decimalsForInterval( double interval )
  {
    if ( interval >= 1.0 )
      return 0;
    return qBound( 0, int( -std::floor( std::log10( interval ) ) ), 8 );
  }
}

// ---------------------------------------------------------------- scale bar

void PaleoScaleBarDecoration::render( const QgsMapSettings &mapSettings, QgsRenderContext &context )
{
  QPainter *painter = context.painter();
  if ( !painter || !painter->device() )
    return;

  const double mupp = mapSettings.mapUnitsPerPixel();
  if ( !( mupp > 0 ) || !std::isfinite( mupp ) )
    return;

  // Pick a round distance for an ~80px bar (upstream preferredSize).
  const double targetPx = 80.0;
  const double dist = niceDistance( mupp * targetPx );
  if ( dist <= 0 )
    return;
  const double barPx = dist / mupp;

  Qgis::DistanceUnit shownUnit = mapSettings.mapUnits();
  double shown = dist;
  if ( shownUnit == Qgis::DistanceUnit::Meters && dist >= 1000.0 )
  {
    shown = dist / 1000.0;
    shownUnit = Qgis::DistanceUnit::Kilometers;
  }
  const QString label = QStringLiteral( "%1 %2" )
                          .arg( QString::number( shown, 'g', 4 ),
                                QgsUnitTypes::toAbbreviatedString( shownUnit ) );

  const int margin = 12;
  const int devH = painter->device()->height();
  const int x0 = margin;
  const int x1 = x0 + int( barPx + 0.5 );
  const int y = devH - margin;

  painter->save();

  const QFont font = painter->font();
  const int textW = painter->fontMetrics().horizontalAdvance( label );

  // Translucent backing panel for legibility over rendered layers.
  painter->setPen( Qt::NoPen );
  painter->setBrush( QColor( 255, 255, 255, 170 ) );
  painter->drawRect( x0 - 5, y - 26,
                     qMax( x1 - x0, textW ) + 10, 34 );

  // Bar + end ticks.
  QPen pen( QColor( 20, 20, 20 ), 2 );
  painter->setPen( pen );
  painter->setBrush( Qt::NoBrush );
  painter->drawLine( QPointF( x0, y ), QPointF( x1, y ) );
  painter->drawLine( QPointF( x0, y ), QPointF( x0, y - 6 ) );
  painter->drawLine( QPointF( x1, y ), QPointF( x1, y - 6 ) );

  // Label above the bar.
  painter->drawText( QPointF( x0 + ( x1 - x0 - textW ) / 2.0, y - 10 ), label );

  painter->setFont( font );
  painter->restore();
}

// ------------------------------------------------------------- north arrow

void PaleoNorthArrowDecoration::render( const QgsMapSettings &mapSettings, QgsRenderContext &context )
{
  QPainter *painter = context.painter();
  if ( !painter || !painter->device() )
    return;

  const double s = 28.0;
  const int margin = 14;
  const double cx = painter->device()->width() - margin - s / 2.0;
  const double cy = margin + s / 2.0;

  painter->save();
  painter->translate( cx, cy );
  painter->rotate( -mapSettings.rotation() );

  // Two-tone arrow: left half dark, right half light (upstream styling).
  const double h = s / 2.0;
  const double w = s * 0.35;
  const QPolygonF leftHalf { QPointF( 0, -h ), QPointF( -w, h ), QPointF( 0, h * 0.55 ) };
  const QPolygonF rightHalf { QPointF( 0, -h ), QPointF( 0, h * 0.55 ), QPointF( w, h ) };

  painter->setPen( QPen( QColor( 20, 20, 20 ), 1 ) );
  painter->setBrush( QColor( 30, 30, 30 ) );
  painter->drawPolygon( leftHalf );
  painter->setBrush( QColor( 240, 240, 240 ) );
  painter->drawPolygon( rightHalf );

  painter->restore();

  // "N" label below the arrow (unrotated, anchored to the viewport corner).
  painter->save();
  painter->setPen( QColor( 20, 20, 20 ) );
  const QString n = QStringLiteral( "N" );
  painter->drawText( QPointF( cx - painter->fontMetrics().horizontalAdvance( n ) / 2.0,
                              cy + s / 2.0 + 12.0 ),
                     n );
  painter->restore();
}

// -------------------------------------------------------------------- grid

void PaleoGridDecoration::render( const QgsMapSettings &mapSettings, QgsRenderContext &context )
{
  QPainter *painter = context.painter();
  if ( !painter || !painter->device() )
    return;

  const QgsRectangle ext = mapSettings.extent();
  if ( ext.isEmpty() || !std::isfinite( ext.width() ) || ext.width() <= 0 )
    return;

  const double interval = niceDistance( ext.width() / 6.0 );
  if ( interval <= 0 )
    return;
  const int decimals = decimalsForInterval( interval );

  const QgsMapToPixel &mtp = context.mapToPixel();
  const int devW = painter->device()->width();
  const int devH = painter->device()->height();

  painter->save();
  QPen pen( QColor( 60, 60, 60, 90 ), 0 );
  pen.setStyle( Qt::DashLine );
  painter->setPen( pen );

  const double x0 = std::ceil( ext.xMinimum() / interval ) * interval;
  for ( double x = x0; x <= ext.xMaximum() + interval * 1e-9; x += interval )
  {
    const QgsPointXY px = mtp.transform( QgsPointXY( x, ext.yMinimum() ) );
    painter->drawLine( QPointF( px.x(), 0 ), QPointF( px.x(), devH ) );
    painter->drawText( QPointF( px.x() + 3, devH - 4 ),
                       QStringLiteral( "x=%1" ).arg( x, 0, 'f', decimals ) );
  }

  const double y0 = std::ceil( ext.yMinimum() / interval ) * interval;
  for ( double y = y0; y <= ext.yMaximum() + interval * 1e-9; y += interval )
  {
    const QgsPointXY py = mtp.transform( QgsPointXY( ext.xMinimum(), y ) );
    painter->drawLine( QPointF( 0, py.y() ), QPointF( devW, py.y() ) );
    painter->drawText( QPointF( 4, py.y() - 4 ),
                       QStringLiteral( "y=%1" ).arg( y, 0, 'f', decimals ) );
  }

  painter->restore();
}

// ----------------------------------------------------------------- manager

PaleoDecorationManager::PaleoDecorationManager( QgsMapCanvas *canvas, QObject *parent )
  : QObject( parent ? parent : static_cast<QObject *>( canvas ) )
  , mCanvas( canvas )
  , mScaleBar( std::make_unique<PaleoScaleBarDecoration>() )
  , mNorthArrow( std::make_unique<PaleoNorthArrowDecoration>() )
  , mGrid( std::make_unique<PaleoGridDecoration>() )
{
  // QGIS 4.x has no QgsMapCanvas::addDecorationItem — decorations paint from
  // the post-render hook, same as libqgis_app's QgsDecorationItem.
  connect( mCanvas, &QgsMapCanvas::renderComplete,
           this, &PaleoDecorationManager::paintDecorations );
}

void PaleoDecorationManager::setScaleBarEnabled( bool enabled )
{
  if ( mScaleBarEnabled == enabled )
    return;
  mScaleBarEnabled = enabled;
  mCanvas->refresh();
}

void PaleoDecorationManager::setNorthArrowEnabled( bool enabled )
{
  if ( mNorthArrowEnabled == enabled )
    return;
  mNorthArrowEnabled = enabled;
  mCanvas->refresh();
}

void PaleoDecorationManager::setGridEnabled( bool enabled )
{
  if ( mGridEnabled == enabled )
    return;
  mGridEnabled = enabled;
  mCanvas->refresh();
}

QList<QgsMapDecoration *> PaleoDecorationManager::decorationItems() const
{
  QList<QgsMapDecoration *> items;
  // Grid is map-anchored — paint it under the viewport overlays.
  if ( mGridEnabled )
    items << mGrid.get();
  if ( mScaleBarEnabled )
    items << mScaleBar.get();
  if ( mNorthArrowEnabled )
    items << mNorthArrow.get();
  return items;
}

void PaleoDecorationManager::paintDecorations( QPainter *painter )
{
  if ( !mCanvas || !painter )
    return;

  const QgsMapSettings &ms = mCanvas->mapSettings();
  QgsRenderContext context = QgsRenderContext::fromQPainter( painter );
  context.setMapToPixel( ms.mapToPixel() );
  context.setExtent( ms.visibleExtent() );

  const QList<QgsMapDecoration *> items = decorationItems();
  for ( QgsMapDecoration *d : items )
  {
    painter->save();
    d->render( ms, context );
    painter->restore();
  }
}
