// 层：视图
#include "paleodecorations.h"
#include "../../domain/faciescatalog.h"
#include <QCoreApplication>
#include <QSvgRenderer>

#include <cmath>

#include <QColor>
#include <QEvent>
#include <QFont>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QWidget>

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


namespace PaleoDecorationTheme
{
  namespace
  {
    bool gDark = false;
  }
  void setDark( bool dark ) { gDark = dark; }
  bool isDark() { return gDark; }
  QColor card() { return gDark ? QColor( 27, 33, 42, 200 ) : QColor( 255, 255, 255, 170 ); }
  QColor ink() { return gDark ? QColor( QStringLiteral( "#E4EAF2" ) ) : QColor( 20, 20, 20 ); }
  QColor inkSoft() { return gDark ? QColor( 163, 177, 191, 120 ) : QColor( 60, 60, 60, 90 ); }
  QColor border() { return gDark ? QColor( QStringLiteral( "#3B4552" ) ) : QColor( QStringLiteral( "#DFE5EC" ) ); }
  QColor arrowDarkHalf() { return gDark ? QColor( 228, 234, 242 ) : QColor( 30, 30, 30 ); }
  QColor arrowLightHalf() { return gDark ? QColor( 59, 69, 82 ) : QColor( 240, 240, 240 ); }
} // namespace PaleoDecorationTheme

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
  painter->setBrush( PaleoDecorationTheme::card() );
  painter->drawRect( x0 - 5, y - 26,
                     qMax( x1 - x0, textW ) + 10, 34 );

  // Bar + end ticks.
  QPen pen( PaleoDecorationTheme::ink(), 2 );
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
  const double cx = margin + s / 2.0;
  const double cy = margin + s / 2.0;

  painter->save();
  painter->setPen(Qt::NoPen);
  painter->setBrush( PaleoDecorationTheme::card() );
  painter->drawRoundedRect(QRectF(margin-6, margin-6, s+12, s+28),4,4);
  painter->translate( cx, cy );
  painter->rotate( -mapSettings.rotation() );

  // Two-tone arrow: left half dark, right half light (upstream styling).
  const double h = s / 2.0;
  const double w = s * 0.35;
  const QPolygonF leftHalf { QPointF( 0, -h ), QPointF( -w, h ), QPointF( 0, h * 0.55 ) };
  const QPolygonF rightHalf { QPointF( 0, -h ), QPointF( 0, h * 0.55 ), QPointF( w, h ) };

  painter->setPen( QPen( PaleoDecorationTheme::ink(), 1 ) );
  painter->setBrush( PaleoDecorationTheme::arrowDarkHalf() );
  painter->drawPolygon( leftHalf );
  painter->setBrush( PaleoDecorationTheme::arrowLightHalf() );
  painter->drawPolygon( rightHalf );

  painter->restore();

  // "N" label below the arrow (unrotated, anchored to the viewport corner).
  painter->save();
  painter->setPen( PaleoDecorationTheme::ink() );
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
  QPen pen( PaleoDecorationTheme::inkSoft(), 0 );
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

// ---------------------------------------------------------------- watermark

void PaleoWatermarkDecoration::render( const QgsMapSettings &mapSettings, QgsRenderContext &context )
{
  Q_UNUSED( mapSettings )
  QPainter *painter = context.painter();
  if ( !painter || !painter->device() || mText.isEmpty() )
    return;

  // 顶中胶囊：warning #F29900 底 + 白字——与 DESIGN 的状态用色一致；水印只
  // 在临时配准图层存在期间绘制，平时不出现。
  QFont font = painter->font();
  font.setPointSizeF( 9.0 );
  font.setBold( true );
  painter->setFont( font );
  const QFontMetricsF fm( font );
  const qreal textW = fm.horizontalAdvance( mText );
  const qreal pillW = textW + 20;
  const qreal pillH = fm.height() + 8;
  const qreal x = ( painter->device()->width() - pillW ) / 2.0;
  const qreal y = 10;

  painter->save();
  painter->setRenderHint( QPainter::Antialiasing, true );
  painter->setPen( Qt::NoPen );
  painter->setBrush( QColor( 242, 153, 0, 200 ) ); // warning #F29900 @ ~78%
  painter->drawRoundedRect( QRectF( x, y, pillW, pillH ), pillH / 2.0, pillH / 2.0 );
  painter->setPen( QColor( 255, 255, 255 ) );
  painter->drawText( QRectF( x, y, pillW, pillH ), Qt::AlignCenter, mText );
  painter->restore();
}

namespace
{
  class PaleoDecorationOverlay : public QWidget
  {
  public:
    explicit PaleoDecorationOverlay( QgsMapCanvas *canvas, PaleoDecorationManager *mgr )
      : QWidget( canvas ? canvas->viewport() : nullptr )
      , mCanvas( canvas )
      , mMgr( mgr )
    {
      setObjectName( QStringLiteral( "paleoDecorationOverlay" ) );
      setAttribute( Qt::WA_TransparentForMouseEvents, true );
      setAttribute( Qt::WA_NoSystemBackground, true );
      setAttribute( Qt::WA_TranslucentBackground, true );
      if ( mCanvas && mCanvas->viewport() )
      {
        mCanvas->viewport()->installEventFilter( this );
        resize( mCanvas->viewport()->size() );
        show();
      }
    }

  protected:
    bool eventFilter( QObject *obj, QEvent *ev ) override
    {
      if ( mCanvas && obj == mCanvas->viewport() )
      {
        if ( ev->type() == QEvent::Resize || ev->type() == QEvent::Show )
        {
          resize( mCanvas->viewport()->size() );
          raise();
          update();
        }
      }
      return QWidget::eventFilter( obj, ev );
    }

    void paintEvent( QPaintEvent * ) override
    {
      if ( !mCanvas || !mMgr || mMgr->decorationItems().isEmpty() )
        return;
      QPainter painter( this );
      mMgr->paintDecorations( &painter );
    }

  private:
    QgsMapCanvas *mCanvas = nullptr;
    PaleoDecorationManager *mMgr = nullptr;
  };
}

// ----------------------------------------------------------------- manager

PaleoDecorationManager::PaleoDecorationManager( QgsMapCanvas *canvas, QObject *parent )
  : QObject( parent ? parent : static_cast<QObject *>( canvas ) )
  , mCanvas( canvas )
  , mLegend( std::make_unique<PaleoFaciesLegendDecoration>() )
  , mScaleBar( std::make_unique<PaleoScaleBarDecoration>() )
  , mNorthArrow( std::make_unique<PaleoNorthArrowDecoration>() )
  , mGrid( std::make_unique<PaleoGridDecoration>() )
  , mWatermark( std::make_unique<PaleoWatermarkDecoration>() )
{
  // 水印默认文案走翻译机制（装饰类非 QObject，语境挂管理器）。
  mWatermark->setText( QCoreApplication::translate( "PaleoDecorationManager", "临时配准 · 手工仿射" ) );
  if ( mCanvas && mCanvas->viewport() )
  {
    mOverlay = new PaleoDecorationOverlay( mCanvas, this );
    connect( mCanvas, &QgsMapCanvas::mapCanvasRefreshed,
             this, [this]() {
               if ( mOverlay )
               {
                 mOverlay->raise();
                 mOverlay->update();
               }
             } );
    connect( mCanvas, &QgsMapCanvas::extentsChanged,
             this, [this]() {
               if ( mOverlay )
               {
                 mOverlay->raise();
                 mOverlay->update();
               }
             } );
  }

  // QGIS 4.x has no QgsMapCanvas::addDecorationItem — decorations paint from
  // the post-render hook, same as libqgis_app's QgsDecorationItem.
  // Screen decorations belong to the foreground overlay only. Baking another
  // copy into the cached map image leaves old legends visible after a schema
  // or horizon change. Exporters can render decorationItems() explicitly.
  if (!mOverlay)
    connect(mCanvas, &QgsMapCanvas::renderComplete, this,
            &PaleoDecorationManager::paintDecorations);
}

void PaleoDecorationManager::setScaleBarEnabled( bool enabled )
{
  if ( mScaleBarEnabled == enabled )
    return;
  mScaleBarEnabled = enabled;
  if ( mOverlay )
  {
    mOverlay->raise();
    mOverlay->update();
  }
  mCanvas->refresh();
}

void PaleoDecorationManager::setNorthArrowEnabled( bool enabled )
{
  if ( mNorthArrowEnabled == enabled )
    return;
  mNorthArrowEnabled = enabled;
  if ( mOverlay )
  {
    mOverlay->raise();
    mOverlay->update();
  }
  mCanvas->refresh();
}

void PaleoDecorationManager::setGridEnabled( bool enabled )
{
  if ( mGridEnabled == enabled )
    return;
  mGridEnabled = enabled;
  if ( mOverlay )
  {
    mOverlay->raise();
    mOverlay->update();
  }
  mCanvas->refresh();
}

void PaleoDecorationManager::setWatermarkEnabled( bool enabled )
{
  if ( mWatermarkEnabled == enabled )
    return;
  mWatermarkEnabled = enabled;
  if ( mOverlay )
  {
    mOverlay->raise();
    mOverlay->update();
  }
  mCanvas->refresh();
}

void PaleoDecorationManager::setWatermarkText( const QString &text )
{
  mWatermark->setText( text );
  if ( mWatermarkEnabled )
  {
    if ( mOverlay )
    {
      mOverlay->raise();
      mOverlay->update();
    }
    mCanvas->refresh();
  }
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
  if ( mLegendEnabled )
    items << mLegend.get();
  if ( mWatermarkEnabled )
    items << mWatermark.get();
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

void PaleoFaciesLegendDecoration::render(const QgsMapSettings &, QgsRenderContext &context)
{
  auto *p=context.painter();if(!p || !p->device())return;
  p->save();QFont f=p->font();f.setPointSize(8);p->setFont(f);
  const int row=p->fontMetrics().height()+8;
  const int width=std::min(240, std::max(140, p->device()->width()/3));
  const int x=p->device()->width()-width-16,y=16;
  p->setPen(PaleoDecorationTheme::border());p->setBrush(PaleoDecorationTheme::card());
  p->drawRoundedRect(QRectF(x,y,width,16+row*(facies.size()+1)),4,4);
  p->setPen(PaleoDecorationTheme::ink());f.setBold(true);p->setFont(f);
  p->drawText(QRect(x+8,y+4,width-16,row),Qt::AlignVCenter,p->fontMetrics().elidedText(title,Qt::ElideRight,width-16));
  f.setBold(false);p->setFont(f);
  for(int i=0;i<facies.size();++i){const auto entry=facies[i].toMap();int top=y+8+(i+1)*row;
    p->setPen(PaleoDecorationTheme::border());p->setBrush(QColor(entry.value("color").toString()));p->drawRect(QRect(x+8,top+3,16,row-8));
    const auto texture =
        FaciesCatalog::resourcePath(entry.value("texture").toString());
    if (!texture.isEmpty()) {
      QSvgRenderer svg(texture);
      svg.render(p, QRectF(x + 8, top + 3, 16, row - 8));
    }
    p->setPen(PaleoDecorationTheme::ink());const auto label=entry.value("name").toString();p->drawText(QRect(x+32,top,width-40,row),Qt::AlignVCenter,p->fontMetrics().elidedText(label,Qt::ElideRight,width-40));}
  p->restore();
}
void PaleoDecorationManager::setFaciesLegend(const QString &title,const QVariantList &facies)
{
  mLegend->title=title;mLegend->facies=facies;mLegendEnabled=true;
  if(mOverlay){mOverlay->raise();mOverlay->update();}if(mCanvas)mCanvas->refresh();
}
