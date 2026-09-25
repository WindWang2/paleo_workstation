#include "workflows.h"

#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QVariantList>

#include <qgsmaplayer.h>

// ---------------------------------------------------------------------------
// workflows.h fixes the public shape of these classes and declares no data
// members. Service bindings and per-instance state therefore ride on dynamic
// QObject properties — QObject owns the storage, so nothing needs manual
// cleanup when a workflow is destroyed.
// ---------------------------------------------------------------------------

namespace
{
  const char kProcProp[]        = "paleo.wf.proc";        // QObject* (QgisProcessingService)
  const char kLayersProp[]      = "paleo.wf.layers";      // QObject* (QgisLayerService)
  const char kStoreProp[]       = "paleo.wf.store";       // QObject* (PaleoProjectStore)
  const char kConstraintsProp[] = "paleo.wf.constraints"; // QVariantList of QVariantMap (Constraint::toMap + "horizon")
  const char kSeqProp[]         = "paleo.wf.seq";         // int — constraint id sequence

  void setError( QString *error, const QString &text )
  {
    if ( error )
      *error = text;
  }

  QgisProcessingService *procOf( const QObject *wf )
  {
    return qobject_cast<QgisProcessingService *>( wf->property( kProcProp ).value<QObject *>() );
  }

  QgisLayerService *layersOf( const QObject *wf )
  {
    return qobject_cast<QgisLayerService *>( wf->property( kLayersProp ).value<QObject *>() );
  }

  PaleoProjectStore *storeOf( const QObject *wf )
  {
    return qobject_cast<PaleoProjectStore *>( wf->property( kStoreProp ).value<QObject *>() );
  }

  void bindProcessing( QObject *wf, QgisProcessingService *proc, QgisLayerService *layers )
  {
    wf->setProperty( kProcProp, QVariant::fromValue( static_cast<QObject *>( proc ) ) );
    wf->setProperty( kLayersProp, QVariant::fromValue( static_cast<QObject *>( layers ) ) );
  }

  // Compact UTC timestamp — makes result layer ids unique per run.
  QString stamp()
  {
    return QDateTime::currentDateTimeUtc().toString( QStringLiteral( "yyyyMMdd-hhmmss-zzz" ) );
  }

  // Unpinned processing destinations land in the system temp dir; the merge
  // step (PaleoProjectStore write queue) promotes them into the project later.
  QString tempRasterPath( const QString &tag, const QString &horizon )
  {
    return QDir::temp().filePath(
        QStringLiteral( "paleo_%1_%2_%3.tif" ).arg( tag, horizon, stamp() ) );
  }

  // OUTPUT is the conventional destination key; fall back to the first
  // string-valued result so algorithms with differently named destinations
  // still declare their product.
  QString outputPathOf( const QVariantMap &results )
  {
    QString out = results.value( QStringLiteral( "OUTPUT" ) ).toString();
    if ( out.isEmpty() )
    {
      for ( const QVariant &v : results )
      {
        if ( v.typeId() == QMetaType::QString && !v.toString().isEmpty() )
        {
          out = v.toString();
          break;
        }
      }
    }
    return out;
  }

  // A declared source counts as "file-backed" when its base (before any
  // "|layername=" etc. suffix) is a plain filesystem path. Memory pseudo-URIs
  // and remote/VSI sources are not QFile-checkable and are skipped.
  bool isFileBackedSource( const QString &source )
  {
    if ( source.isEmpty() )
      return false;
    const QString base = source.section( QLatin1Char( '|' ), 0, 0 );
    if ( base.isEmpty() )
      return false;
    if ( base.startsWith( QStringLiteral( "memory" ), Qt::CaseInsensitive ) )
      return false;
    if ( base.contains( QLatin1Char( '?' ) ) )          // memory provider URI ("Point?crs=...")
      return false;
    if ( base.contains( QStringLiteral( "://" ) ) )     // remote URI
      return false;
    if ( base.startsWith( QStringLiteral( "/vsi" ) ) )  // GDAL virtual filesystem
      return false;
    return true;
  }
} // namespace

// ---------------------------------------------------------------------------
// PredictionWorkflow — ①智能预测
// ---------------------------------------------------------------------------

PredictionWorkflow::PredictionWorkflow( QgisProcessingService *proc, QgisLayerService *layers, QObject *parent )
  : QObject( parent )
{
  bindProcessing( this, proc, layers );
}

bool PredictionWorkflow::runPrediction( const QString &horizon, const QString &algorithmId,
                                        const QVariantMap &params, QString *error )
{
  QgisProcessingService *proc = procOf( this );
  QgisLayerService *layers = layersOf( this );
  if ( !proc || !layers )
  {
    const QString msg = tr( "prediction workflow is not bound to services" );
    setError( error, msg );
    emit predictionFailed( horizon, msg );
    return false;
  }

  const QVariantMap results = proc->run( algorithmId, params, error );
  if ( results.isEmpty() )
  {
    QString msg = tr( "prediction algorithm '%1' produced no results" ).arg( algorithmId );
    if ( error && !error->isEmpty() )
      msg = *error;
    else
      setError( error, msg );
    emit predictionFailed( horizon, msg );
    return false;
  }

  const QString outPath = outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    const QString msg = tr( "prediction algorithm '%1' returned no output path" ).arg( algorithmId );
    setError( error, msg );
    emit predictionFailed( horizon, msg );
    return false;
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "predict.%1.%2" ).arg( horizon, stamp() );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = outPath;
  decl.group = QStringLiteral( "01_Prediction" );
  if ( !layers->declare( decl, error ) )
  {
    emit predictionFailed( horizon,
        ( error && !error->isEmpty() )
            ? *error
            : tr( "failed to declare result layer '%1'" ).arg( decl.layerId ) );
    return false;
  }

  emit predictionDone( horizon, decl.layerId );
  return true;
}

// ---------------------------------------------------------------------------
// ConstraintWorkflow — ②约束与单因素
// ---------------------------------------------------------------------------

ConstraintWorkflow::ConstraintWorkflow( QgisProcessingService *proc, QgisLayerService *layers, QObject *parent )
  : QObject( parent )
{
  bindProcessing( this, proc, layers );
}

bool ConstraintWorkflow::addConstraint( const QString &horizon, const QString &wkt,
                                        const QString &type, int faciesCode, QString *error )
{
  QgisLayerService *layers = layersOf( this );
  if ( !layers )
  {
    setError( error, tr( "constraint workflow is not bound to a layer service" ) );
    return false;
  }
  if ( wkt.trimmed().isEmpty() )
  {
    setError( error, tr( "constraint geometry WKT is empty" ) );
    return false;
  }

  // In-memory constraint record (member-equivalent state via property).
  Constraint c;
  const int seq = property( kSeqProp ).toInt() + 1;
  setProperty( kSeqProp, seq );
  c.id = QStringLiteral( "c-%1" ).arg( seq );
  c.type = type;
  c.wkt = wkt;
  c.targetFaciesCode = faciesCode;

  QVariantList constraints = property( kConstraintsProp ).toList();
  QVariantMap rec = c.toMap();
  rec.insert( QStringLiteral( "horizon" ), horizon ); // Constraint has no horizon field — tag it here.
  constraints.append( rec );

  // Persist as a per-horizon constraints layer declaration. The memory
  // pseudo-source embeds the accumulated WKT payloads so the geometry set
  // round-trips through the manifest until real constraint storage lands.
  QStringList wkts;
  for ( const QVariant &v : constraints )
  {
    const QVariantMap m = v.toMap();
    if ( m.value( QStringLiteral( "horizon" ) ).toString() == horizon )
      wkts << m.value( QStringLiteral( "wkt" ) ).toString();
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "vector" );
  decl.source = QStringLiteral( "memory|%1" ).arg( wkts.join( QLatin1Char( '|' ) ) );
  decl.group = QStringLiteral( "02_Constraints" );
  if ( !layers->declare( decl, error ) )
  {
    constraints.removeLast(); // keep the in-memory list consistent with the manifest
    setProperty( kConstraintsProp, constraints );
    return false;
  }

  setProperty( kConstraintsProp, constraints );
  emit constraintAdded( c.id );
  return true;
}

bool ConstraintWorkflow::runConstraintIDW( const QString &horizon, const QString &pointsLayerId,
                                          const QString &field, double cellSize, QString *error )
{
  QgisProcessingService *proc = procOf( this );
  QgisLayerService *layers = layersOf( this );
  if ( !proc || !layers )
  {
    setError( error, tr( "constraint workflow is not bound to services" ) );
    return false;
  }

  // INPUT is a QgsProcessingParameterFeatureSource: it accepts a QgsMapLayer*
  // variant, so resolve the declared points layer through the layer service.
  QgsMapLayer *points = layers->instantiate( pointsLayerId, error );
  if ( !points )
    return false;

  QVariantMap params;
  params.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( points ) );
  params.insert( QStringLiteral( "FIELD" ), field );
  params.insert( QStringLiteral( "CELL_SIZE" ), cellSize );
  params.insert( QStringLiteral( "OUTPUT" ), tempRasterPath( QStringLiteral( "idw" ), horizon ) );

  const QVariantMap results = proc->run( QStringLiteral( "paleo:paleo_constraint_idw" ), params, error );
  if ( results.isEmpty() )
    return false;

  const QString outPath = outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    setError( error, tr( "constraint IDW returned no output path" ) );
    return false;
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "factor.%1.idw" ).arg( horizon );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = outPath;
  decl.group = QStringLiteral( "04_SingleFactor" );
  if ( !layers->declare( decl, error ) )
    return false;

  emit factorDone( horizon, decl.layerId );
  return true;
}

// ---------------------------------------------------------------------------
// CompositionWorkflow — ③综合编图
// ---------------------------------------------------------------------------

CompositionWorkflow::CompositionWorkflow( QgisProcessingService *proc, QgisLayerService *layers, QObject *parent )
  : QObject( parent )
{
  bindProcessing( this, proc, layers );
}

bool CompositionWorkflow::fuseFactors( const QString &horizon, const QStringList &factorLayerIds,
                                       QString *error )
{
  QgisProcessingService *proc = procOf( this );
  QgisLayerService *layers = layersOf( this );
  if ( !proc || !layers )
  {
    setError( error, tr( "composition workflow is not bound to services" ) );
    return false;
  }
  if ( factorLayerIds.isEmpty() )
  {
    setError( error, tr( "no factor layers supplied for fusion" ) );
    return false;
  }

  QVariantList inputs;
  inputs.reserve( factorLayerIds.size() );
  for ( const QString &layerId : factorLayerIds )
  {
    QgsMapLayer *layer = layers->instantiate( layerId, error );
    if ( !layer )
      return false;
    inputs.append( QVariant::fromValue( layer ) );
  }

  QVariantMap params;
  params.insert( QStringLiteral( "INPUTS" ), inputs );
  params.insert( QStringLiteral( "OUTPUT" ), tempRasterPath( QStringLiteral( "fusion" ), horizon ) );

  const QVariantMap results = proc->run( QStringLiteral( "paleo:paleo_facies_fusion" ), params, error );
  if ( results.isEmpty() )
    return false;

  const QString outPath = outputPathOf( results );
  if ( outPath.isEmpty() )
  {
    setError( error, tr( "facies fusion returned no output path" ) );
    return false;
  }

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "composite.%1" ).arg( horizon );
  decl.horizon = horizon;
  decl.type = QStringLiteral( "raster" );
  decl.source = outPath;
  decl.group = QStringLiteral( "03_Composite" );
  if ( !layers->declare( decl, error ) )
    return false;

  emit compositionDone( horizon, decl.layerId );
  return true;
}

// ---------------------------------------------------------------------------
// ValidationWorkflow — ④验证
// ---------------------------------------------------------------------------

ValidationWorkflow::ValidationWorkflow( QgisLayerService *layers, PaleoProjectStore *store, QObject *parent )
  : QObject( parent )
{
  setProperty( kLayersProp, QVariant::fromValue( static_cast<QObject *>( layers ) ) );
  setProperty( kStoreProp, QVariant::fromValue( static_cast<QObject *>( store ) ) );
}

QList<ValidationIssue> ValidationWorkflow::validate()
{
  QList<ValidationIssue> issues;
  QgisLayerService *layers = layersOf( this );
  PaleoProjectStore *store = storeOf( this );
  const QVector<LayerDeclaration> decls = layers ? layers->declared() : QVector<LayerDeclaration>();

  // (a) declared raster/vector layers whose file source is missing on disk.
  for ( const LayerDeclaration &d : decls )
  {
    const bool fileType = d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) == 0 ||
                          d.type.compare( QStringLiteral( "vector" ), Qt::CaseInsensitive ) == 0;
    if ( !fileType || !isFileBackedSource( d.source ) )
      continue;
    const QString base = d.source.section( QLatin1Char( '|' ), 0, 0 );
    if ( !QFile::exists( base ) )
    {
      ValidationIssue v;
      v.severity = ValidationIssue::Error;
      v.code = QStringLiteral( "SRC_MISSING" );
      v.message = tr( "declared layer '%1' source is missing on disk: %2" ).arg( d.layerId, base );
      v.layerId = d.layerId;
      v.horizon = d.horizon;
      issues.append( v );
    }
  }

  // (b) duplicate horizon names. The manifest's horizons() set is DISTINCT, so
  // an exact duplicate cannot occur; what slips through is a collision after
  // normalization (case / stray whitespace) — e.g. "T1" vs "t1".
  {
    QSet<QString> raw;
    for ( const LayerDeclaration &d : decls )
      if ( !d.horizon.isEmpty() )
        raw.insert( d.horizon );

    QHash<QString, QStringList> byNormalized;
    for ( const QString &h : raw )
      byNormalized[h.trimmed().toLower()].append( h );

    for ( auto it = byNormalized.constBegin(); it != byNormalized.constEnd(); ++it )
    {
      if ( it.value().size() < 2 )
        continue;
      QStringList names = it.value();
      names.sort();
      ValidationIssue v;
      v.severity = ValidationIssue::Warning;
      v.code = QStringLiteral( "DUP_HORIZON" );
      v.message = tr( "horizon names collide: %1" ).arg( names.join( QStringLiteral( " / " ) ) );
      v.horizon = names.first();
      issues.append( v );
    }
  }

  // (c) busy-layer conflicts — any declared layer still held by a running task.
  if ( store )
  {
    for ( const LayerDeclaration &d : decls )
    {
      QString reason;
      if ( store->layerBusy( d.layerId, &reason ) )
      {
        ValidationIssue v;
        v.severity = ValidationIssue::Info;
        v.code = QStringLiteral( "BUSY" );
        v.message = tr( "layer '%1' is busy: %2" ).arg( d.layerId, reason );
        v.layerId = d.layerId;
        v.horizon = d.horizon;
        issues.append( v );
      }
    }
  }

  emit validationDone( issues.size() );
  return issues;
}
