#include <qgsfeedback.h>

// ---------------------------------------------------------------------------
// Compat shim: the frozen paleolocatorfilters.h declares fetchResults() taking
// QgsLocatorFeedback*, but QGIS 4.2 renamed that type to QgsFeedback — the
// base-class signature in qgslocatorfilter.h is
//   fetchResults( const QString &, const QgsLocatorContext &, QgsFeedback * )
// Aliasing keeps the header's `override` declarations valid. This must appear
// before the header include in every TU that pulls it in (see tst_locator.cpp;
// AUTOMOC-generated moc files need the same shim via -include or a header fix).
// ---------------------------------------------------------------------------
using QgsLocatorFeedback = QgsFeedback;

#include "paleolocatorfilters.h"

#include <qgsfeature.h>
#include <qgsfeatureid.h>
#include <qgsfeaturerequest.h>
#include <qgsgeometry.h>
#include <qgsmapcanvas.h>
#include <qgsvectorlayer.h>

#include <QVariantMap>

namespace
{
  // Cap emitted results so a broad query over a big layer can't flood the
  // locator model.
  constexpr int MAX_RESULTS = 50;

  bool matches( const QString &candidate, const QString &query )
  {
    return candidate.contains( query, Qt::CaseInsensitive );
  }
}

// ---------------------------------------------------------------------------
// WellLocatorFilter — search well ids (and a conventional "name" field when
// present) in the wells point layer; trigger selects + zooms + flashes.
// ---------------------------------------------------------------------------

WellLocatorFilter::WellLocatorFilter( WellLayerProvider provider, QgsMapCanvas *canvas, QObject *parent )
  : QgsLocatorFilter( parent )
  , m_provider( std::move( provider ) )
  , m_canvas( canvas )
{
}

void WellLocatorFilter::fetchResults( const QString &string, const QgsLocatorContext &context,
                                      QgsLocatorFeedback *feedback )
{
  Q_UNUSED( context )
  if ( string.trimmed().isEmpty() || !m_provider )
    return;

  const QPair<QgsVectorLayer *, QString> resolved = m_provider();
  QgsVectorLayer *layer = resolved.first;
  const QString idField = resolved.second;
  if ( !layer || !layer->isValid() || idField.isEmpty() )
    return;

  // Display-substring match against the declared id field, plus a
  // conventional "name" field when the layer carries one.
  QStringList matchFields;
  if ( layer->fields().lookupField( idField ) >= 0 )
    matchFields << idField;
  const QString nameField = QStringLiteral( "name" );
  if ( !matchFields.contains( nameField, Qt::CaseInsensitive )
       && layer->fields().lookupField( nameField ) >= 0 )
    matchFields << nameField;
  if ( matchFields.isEmpty() )
    return;

  QgsFeatureRequest request;
  request.setSubsetOfAttributes( matchFields, layer->fields() );

  QgsFeatureIterator it = layer->getFeatures( request );
  QgsFeature f;
  int emitted = 0;
  while ( it.nextFeature( f ) )
  {
    if ( feedback && feedback->isCanceled() )
      break;

    bool hit = false;
    for ( const QString &field : matchFields )
    {
      if ( matches( f.attribute( field ).toString(), string ) )
      {
        hit = true;
        break;
      }
    }
    if ( !hit )
      continue;

    QgsLocatorResult result;
    result.filter = this;
    result.displayString = f.attribute( idField ).toString();
    if ( result.displayString.isEmpty() )
      result.displayString = FID_TO_STRING( f.id() );

    QVariantMap data;
    data.insert( QStringLiteral( "fid" ), FID_TO_NUMBER( f.id() ) );
    data.insert( QStringLiteral( "wkt" ),
                 f.hasGeometry() ? f.geometry().asWkt() : QString() );
    result.setUserData( data );
    emit resultFetched( result );

    if ( ++emitted >= MAX_RESULTS )
      break;
  }
}

void WellLocatorFilter::triggerResult( const QgsLocatorResult &result )
{
  const QVariantMap data = result.userData().toMap();
  if ( !data.contains( QStringLiteral( "fid" ) ) || !m_provider )
    return;
  const QgsFeatureId fid = data.value( QStringLiteral( "fid" ) ).toLongLong();

  QgsVectorLayer *layer = m_provider().first;
  if ( !layer || !layer->isValid() )
    return;

  const QgsFeatureIds ids{ fid };
  layer->selectByIds( ids );
  if ( m_canvas )
  {
    // QGIS 4.2: zoomToFeatureExtent() takes a bare QgsRectangle; the
    // layer+ids entry point is zoomToFeatureIds().
    m_canvas->zoomToFeatureIds( layer, ids );
    m_canvas->flashFeatureIds( layer, ids );
    m_canvas->refresh();
  }
}

// ---------------------------------------------------------------------------
// HorizonLocatorFilter — contains-match over manifest horizon ids; trigger
// switches the active horizon via the injected callback.
// ---------------------------------------------------------------------------

HorizonLocatorFilter::HorizonLocatorFilter( HorizonListProvider provider, ActivateFn activate, QObject *parent )
  : QgsLocatorFilter( parent )
  , m_provider( std::move( provider ) )
  , m_activate( std::move( activate ) )
{
}

void HorizonLocatorFilter::fetchResults( const QString &string, const QgsLocatorContext &context,
                                         QgsLocatorFeedback *feedback )
{
  Q_UNUSED( context )
  if ( string.trimmed().isEmpty() || !m_provider )
    return;

  int emitted = 0;
  const QStringList ids = m_provider();
  for ( const QString &id : ids )
  {
    if ( feedback && feedback->isCanceled() )
      break;
    if ( !matches( id, string ) )
      continue;

    QgsLocatorResult result;
    result.filter = this;
    result.displayString = id;
    result.setUserData( id );
    emit resultFetched( result );

    if ( ++emitted >= MAX_RESULTS )
      break;
  }
}

void HorizonLocatorFilter::triggerResult( const QgsLocatorResult &result )
{
  if ( m_activate )
    m_activate( result.userData().toString() );
}

// ---------------------------------------------------------------------------
// IssueLocatorFilter — match validation issue codes/messages; trigger hands a
// reconstructed IssueRef to the injected locate callback.
// ---------------------------------------------------------------------------

IssueLocatorFilter::IssueLocatorFilter( IssueProvider provider, LocateFn locate, QObject *parent )
  : QgsLocatorFilter( parent )
  , m_provider( std::move( provider ) )
  , m_locate( std::move( locate ) )
{
}

void IssueLocatorFilter::fetchResults( const QString &string, const QgsLocatorContext &context,
                                       QgsLocatorFeedback *feedback )
{
  Q_UNUSED( context )
  if ( string.trimmed().isEmpty() || !m_provider )
    return;

  int emitted = 0;
  const QList<IssueRef> issues = m_provider();
  for ( const IssueRef &ref : issues )
  {
    if ( feedback && feedback->isCanceled() )
      break;
    if ( !matches( ref.code, string ) && !matches( ref.message, string ) )
      continue;

    QgsLocatorResult result;
    result.filter = this;
    result.displayString = ref.message.isEmpty()
                             ? ref.code
                             : QStringLiteral( "%1 — %2" ).arg( ref.code, ref.message );

    QVariantMap data;
    data.insert( QStringLiteral( "code" ), ref.code );
    data.insert( QStringLiteral( "message" ), ref.message );
    data.insert( QStringLiteral( "layerId" ), ref.layerId );
    data.insert( QStringLiteral( "wkt" ), ref.wkt );
    result.setUserData( data );
    emit resultFetched( result );

    if ( ++emitted >= MAX_RESULTS )
      break;
  }
}

void IssueLocatorFilter::triggerResult( const QgsLocatorResult &result )
{
  if ( !m_locate )
    return;

  const QVariantMap data = result.userData().toMap();
  IssueRef ref;
  ref.code = data.value( QStringLiteral( "code" ) ).toString();
  ref.message = data.value( QStringLiteral( "message" ) ).toString();
  ref.layerId = data.value( QStringLiteral( "layerId" ) ).toString();
  ref.wkt = data.value( QStringLiteral( "wkt" ) ).toString();
  m_locate( ref );
}

// AUTOMOC: the header's Q_OBJECT classes get their metaobjects here — inside
// this TU the QgsLocatorFeedback alias above is already in scope, so the
// generated moc file compiles against the frozen header unmodified.
#include "moc_paleolocatorfilters.cpp"
