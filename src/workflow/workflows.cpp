#include "workflows.h"

#include "../ai/onnxpredictionservice.h" // ORT-free header; symbol refs are PALEO_HAVE_ORT-guarded
#include "../catalog/datacatalog.h"      // localGridCrsWkt — ONNX 栅格落在局部测网
#include "../io/constraintstore.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../services/projectdata.h"
#include "mappingworkflow.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QVariantList>

#include <qgscoordinatereferencesystem.h>
#include <qgsmaplayer.h>

#include <gdal.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
#include <cstring>

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
  const char kOnnxProp[]        = "paleo.wf.onnx";        // QObject* (PaleoOnnxService)
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

  const char kConstraintStoreProp[]      = "paleo.wf.constraintstore";      // void* (ConstraintStore*)
  const char kOwnedConstraintStoreProp[] = "paleo.wf.owned_constraintstore"; // void* (ConstraintStore*)

  PaleoProjectStore *storeOf( const QObject *wf )
  {
    return qobject_cast<PaleoProjectStore *>( wf->property( kStoreProp ).value<QObject *>() );
  }

  ConstraintStore *constraintStoreOf( const QObject *wf )
  {
    QVariant v = wf->property( kConstraintStoreProp );
    if ( v.isValid() && v.value<void *>() )
      return static_cast<ConstraintStore *>( v.value<void *>() );

    PaleoProjectStore *store = storeOf( wf );
    if ( store && !store->gpkgPath().isEmpty() )
    {
      QVariant ov = wf->property( kOwnedConstraintStoreProp );
      if ( ov.isValid() && ov.value<void *>() )
      {
        auto *owned = static_cast<ConstraintStore *>( ov.value<void *>() );
        if ( owned->gpkgPath() == store->gpkgPath() )
          return owned;
        delete owned;
      }
      auto *owned = new ConstraintStore( store->gpkgPath(), store );
      const_cast<QObject *>( wf )->setProperty( kOwnedConstraintStoreProp, QVariant::fromValue( static_cast<void *>( owned ) ) );
      return owned;
    }
    return nullptr;
  }

#if PALEO_HAVE_ORT
  // PaleoOnnxService methods exist only when the vendored runtime is linked;
  // every call site below is guarded the same way so !ORT builds still link.
  PaleoOnnxService *onnxOf( const QObject *wf )
  {
    return qobject_cast<PaleoOnnxService *>( wf->property( kOnnxProp ).value<QObject *>() );
  }
#endif

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

  QString tempVectorPath( const QString &tag, const QString &horizon )
  {
    return QDir::temp().filePath(
        QStringLiteral( "paleo_%1_%2_%3.gpkg" ).arg( tag, horizon, stamp() ) );
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
#if PALEO_HAVE_ORT
  // project_area 工区网格（PROJECT_AREA_PLAN §3 + autoplan eng trap）：ONNX 结果
  // 落在 D61 的 411×641 栅格上，北向上 geotransform (0, 12793/640, 0, 16406, 0,
  // -16406/410)。清单里已声明的 horizon.D61* 栅格优先提供 geotransform 与 SRS，
  // 读不到时用这组常量；行列数硬要求恒为 411×641（plan §1：不是就失败）。
  constexpr int kOnnxGridRows = 411;
  constexpr int kOnnxGridCols = 641;

  struct OnnxAreaGrid
  {
    // (xmin, dx, 0, ymax, 0, -dy) — 原点是左上角像元的外角。
    double gt[6] = { 0.0, 12793.0 / 640.0, 0.0, 16406.0, 0.0, -16406.0 / 410.0 };
    QString projection; // 声明的 D61 栅格自身 SRS（可读时优先于参数/常量）
    bool fromDecl = false;
  };

  // 从图层清单取 horizon.D61* 栅格声明的网格几何；声明缺失或文件不可读时
  // 保持常量（常量本来就是同一套 D61 geotransform）。
  OnnxAreaGrid onnxAreaGrid( const QgisLayerService *layers )
  {
    OnnxAreaGrid grid;
    if ( !layers )
      return grid;
    QString source;
    for ( const LayerDeclaration &d : layers->declared() )
    {
      if ( !d.layerId.startsWith( QLatin1String( "horizon.D61" ) ) ||
           d.type.compare( QStringLiteral( "raster" ), Qt::CaseInsensitive ) != 0 )
        continue;
      source = d.source;
      break;
    }
    if ( source.isEmpty() )
      return grid;
    const QString path = source.section( QLatin1Char( '|' ), 0, 0 );
    if ( path.isEmpty() || !QFile::exists( path ) )
      return grid;
    GDALAllRegister();
    GDALDatasetH ds = GDALOpen( path.toUtf8().constData(), GA_ReadOnly );
    if ( !ds )
      return grid;
    double gt[6] = { 0, 0, 0, 0, 0, 0 };
    if ( GDALGetGeoTransform( ds, gt ) == CE_None )
    {
      std::memcpy( grid.gt, gt, sizeof( gt ) );
      grid.fromDecl = true;
    }
    const char *proj = GDALGetProjectionRef( ds );
    if ( proj && *proj )
      grid.projection = QString::fromUtf8( proj );
    GDALClose( ds );
    return grid;
  }

  // 挤成二维（去掉全部长度-1 维）后必须正好是工区网格 411×641，否则不写栅格、
  // 不登记图层——plan 钉死的文案，附实际行列数（标量/点结果同样拒绝）。
  bool resolveOnnxGrid( const QVector<float> &values, const QVector<int64_t> &outShape,
                        int &rows, int &cols, QString *error )
  {
    const qsizetype n = values.size();
    if ( n <= 0 )
    {
      setError( error, QObject::tr( "ONNX output is empty" ) );
      return false;
    }
    QVector<qint64> dims;
    for ( qint64 d : outShape )
    {
      if ( d != 1 )
        dims.append( d );
    }
    // 标量（shape [] 或全 1）按 1×1 报告实际行列数。
    QStringList parts;
    for ( qint64 d : dims )
      parts << QString::number( d );
    const QString actual = parts.isEmpty() && n == 1 ? QStringLiteral( "1×1" )
                                                     : parts.join( QStringLiteral( "×" ) );
    if ( dims.size() == 2 && dims[0] > 0 && dims[1] > 0 &&
         dims[0] * dims[1] == static_cast<qint64>( n ) )
    {
      rows = static_cast<int>( dims[0] );
      cols = static_cast<int>( dims[1] );
    }
    else
    {
      rows = ( n == 1 ) ? 1 : 0;
      cols = ( n == 1 ) ? 1 : 0;
    }
    if ( rows != kOnnxGridRows || cols != kOnnxGridCols )
    {
      setError( error, QObject::tr( "结果不是 411×641，没有写入栅格（实际 %1）" )
                           .arg( actual.isEmpty() ? QString::number( n ) : actual ) );
      return false;
    }
    return true;
  }

  // 把通过门禁的 411×641 结果写成 Float32 GeoTIFF，geotransform/SRS 用
  // OnnxAreaGrid（D61 声明优先，否则 §3 常量 + 局部测网 CRS）。
  QString writeOnnxRaster( const QString &path, const QVector<float> &values, int rows, int cols,
                          const OnnxAreaGrid &grid, const QVariantMap &params,
                          const QString &model, const QVector<int64_t> &outShape,
                          QString *error )
  {
    GDALAllRegister();
    GDALDriverH drv = GDALGetDriverByName( "GTiff" );
    if ( !drv )
    {
      setError( error, QObject::tr( "GTiff driver is not available" ) );
      return QString();
    }
    if ( QFile::exists( path ) )
      QFile::remove( path );
    GDALDatasetH ds = GDALCreate( drv, path.toUtf8().constData(), cols, rows, 1, GDT_Float32, nullptr );
    if ( !ds )
    {
      setError( error, QObject::tr( "cannot create prediction raster %1" ).arg( path ) );
      return QString();
    }
    double gt[6];
    std::memcpy( gt, grid.gt, sizeof( gt ) );
    GDALSetGeoTransform( ds, gt );
    // SRS 优先级：显式参数 > D61 栅格自身投影 > 局部测网 CRS（与 horizonbinner
    // 一致；绝不把局部米写成经纬度）。
    const QString auth = params.value( QStringLiteral( "crs" ) ).toString();
    if ( !auth.isEmpty() )
    {
      const QgsCoordinateReferenceSystem crs( auth );
      if ( crs.isValid() )
      {
        const QByteArray wkt = crs.toWkt( Qgis::CrsWktVariant::Wkt1Gdal ).toUtf8();
        GDALSetProjection( ds, wkt.constData() );
      }
    }
    else if ( !grid.projection.isEmpty() )
    {
      GDALSetProjection( ds, grid.projection.toUtf8().constData() );
    }
    else
    {
      OGRSpatialReference srs;
      if ( srs.SetFromUserInput( DataCatalog::localGridCrsWkt().toUtf8().constData() ) == OGRERR_NONE )
      {
        char *wkt = nullptr;
        if ( srs.exportToWkt( &wkt ) == OGRERR_NONE && wkt )
          GDALSetProjection( ds, wkt );
        CPLFree( wkt );
      }
    }
    GDALRasterBandH band = GDALGetRasterBand( ds, 1 );
    GDALSetRasterNoDataValue( band, -9999.0 );
    if ( GDALRasterIO( band, GF_Write, 0, 0, cols, rows, const_cast<float *>( values.constData() ),
                       cols, rows, GDT_Float32, 0, 0 ) != CE_None )
    {
      GDALClose( ds );
      QFile::remove( path );
      setError( error, QObject::tr( "failed to write prediction raster %1" ).arg( path ) );
      return QString();
    }

    QJsonObject prov;
    prov.insert( QStringLiteral( "model" ), model );
    prov.insert( QStringLiteral( "rows" ), rows );
    prov.insert( QStringLiteral( "cols" ), cols );
    prov.insert( QStringLiteral( "geotransform_source" ),
                 grid.fromDecl ? QStringLiteral( "horizon.D61" ) : QStringLiteral( "project_area" ) );
    QJsonArray gtJson;
    for ( int i = 0; i < 6; ++i )
      gtJson.append( gt[i] );
    prov.insert( QStringLiteral( "geotransform" ), gtJson );
    QJsonArray shapeJson;
    for ( qint64 d : outShape )
      shapeJson.append( static_cast<double>( d ) );
    prov.insert( QStringLiteral( "output_shape" ), shapeJson );
    const QByteArray json = QJsonDocument( prov ).toJson( QJsonDocument::Compact );
    GDALSetMetadataItem( ds, "PALEO_MODEL", model.toUtf8().constData(), nullptr );
    GDALSetMetadataItem( ds, "PALEO_PROVENANCE", json.constData(), nullptr );
    GDALClose( ds );
    return path;
  }
#endif
} // namespace

// ---------------------------------------------------------------------------
// PredictionWorkflow — ①智能预测
// ---------------------------------------------------------------------------

PredictionWorkflow::PredictionWorkflow( QgisProcessingService *proc, QgisLayerService *layers, QObject *parent )
  : QObject( parent )
{
  bindProcessing( this, proc, layers );
}

void PredictionWorkflow::setOnnxService( PaleoOnnxService *onnx )
{
  setProperty( kOnnxProp, QVariant::fromValue( static_cast<QObject *>( onnx ) ) );
}

QStringList PredictionWorkflow::availableAlgorithms() const
{
  QStringList ids;
  if ( const QgisProcessingService *proc = procOf( this ) )
    ids = proc->paleoAlgorithmIds();
#if PALEO_HAVE_ORT
  if ( const PaleoOnnxService *onnx = onnxOf( this ) )
    for ( const QString &model : onnx->availableModels() )
      ids << QStringLiteral( "onnx:%1" ).arg( model );
#endif
  return ids;
}

bool PredictionWorkflow::runPrediction( const QString &horizon, const QString &algorithmId,
                                        const QVariantMap &params, QString *error )
{
#if PALEO_HAVE_ORT
  // "onnx:<model>" dispatches to PaleoOnnxService; every other id falls
  // through to the Processing-registry path below untouched.
  if ( algorithmId.startsWith( QLatin1String( "onnx:" ) ) )
  {
    const QString model = algorithmId.mid( 5 );
    QgisLayerService *layers = layersOf( this );
    PaleoOnnxService *onnx = onnxOf( this );

    const auto fail = [this, &horizon, error]( const QString &msg ) {
      setError( error, msg );
      emit predictionFailed( horizon, msg );
      return false;
    };
    if ( !layers )
      return fail( tr( "prediction workflow is not bound to a layer service" ) );
    if ( !onnx )
      return fail( tr( "no ONNX service bound; cannot run '%1'" ).arg( algorithmId ) );
    if ( model.isEmpty() )
      return fail( tr( "onnx algorithm id '%1' carries no model name" ).arg( algorithmId ) );
    if ( onnx->loadedModel() != model && !onnx->loadModel( model, error ) )
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to load ONNX model '%1'" ).arg( model ) );

    // params-derived tensors; defaults keep toy-model runs trivial:
    // input {0}, shape {1}, inputName "x".
    QVector<float> input;
    const QVariantList inVals = params.value( QStringLiteral( "input" ) ).toList();
    if ( inVals.isEmpty() )
      input.append( 0.0f );
    else
      for ( const QVariant &v : inVals )
        input.append( v.toFloat() );

    QVector<int64_t> shape;
    const QVariantList shapeVals = params.value( QStringLiteral( "shape" ) ).toList();
    if ( shapeVals.isEmpty() )
      shape.append( 1 );
    else
      for ( const QVariant &v : shapeVals )
        shape.append( static_cast<int64_t>( v.toLongLong() ) );

    QString inputName = params.value( QStringLiteral( "inputName" ) ).toString();
    if ( inputName.isEmpty() )
      inputName = QStringLiteral( "x" );

    const OnnxTensor tensor = onnx->runTensor( inputName, input, shape, error );
    if ( tensor.values.isEmpty() )
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "ONNX model '%1' produced no output" ).arg( model ) );

    // 网格门禁（plan §1 trap）：输出挤成二维后必须是工区网格 411×641，否则
    // 不写栅格、不登记图层。geotransform 由 horizon.D61* 声明提供，缺省常量。
    const OnnxAreaGrid grid = onnxAreaGrid( layers );
    int rows = 0;
    int cols = 0;
    if ( !resolveOnnxGrid( tensor.values, tensor.shape, rows, cols, error ) )
    {
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "cannot place ONNX output on a grid" ) );
    }
    const QString outPath = writeOnnxRaster(
        tempRasterPath( QStringLiteral( "onnx" ), horizon ), tensor.values, rows, cols, grid,
        params, model, tensor.shape, error );
    if ( outPath.isEmpty() )
    {
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to write ONNX prediction raster" ) );
    }

    LayerDeclaration decl;
    decl.layerId = QStringLiteral( "pred.%1.onnx.%2" ).arg( horizon, model );
    decl.horizon = horizon;
    decl.type = QStringLiteral( "raster" );
    decl.source = outPath;
    decl.group = QStringLiteral( "03_Predict" );
    if ( !layers->declare( decl, error ) )
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to declare result layer '%1'" ).arg( decl.layerId ) );

    emit predictionDone( horizon, decl.layerId );
    return true;
  }
#else
  if ( algorithmId.startsWith( QLatin1String( "onnx:" ) ) )
  {
    const QString msg = tr( "cannot run '%1': this build lacks ONNX Runtime" ).arg( algorithmId );
    setError( error, msg );
    emit predictionFailed( horizon, msg );
    return false;
  }
#endif

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
  connect( this, &QObject::destroyed, [this]() {
    QVariant ov = property( kOwnedConstraintStoreProp );
    if ( ov.isValid() && ov.value<void *>() )
      delete static_cast<ConstraintStore *>( ov.value<void *>() );
  } );
}

void ConstraintWorkflow::setConstraintStore( ConstraintStore *store )
{
  setProperty( kConstraintStoreProp, QVariant::fromValue( static_cast<void *>( store ) ) );
}

void ConstraintWorkflow::setStore( PaleoProjectStore *store )
{
  setProperty( kStoreProp, QVariant::fromValue( static_cast<QObject *>( store ) ) );
}

ConstraintStore *ConstraintWorkflow::constraintStore() const
{
  return constraintStoreOf( this );
}

bool ConstraintWorkflow::addConstraint( const QString &horizon, const QString &wkt,
                                        const QString &type, int faciesCode, QString *error,
                                        QString *constraintIdOut )
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

  ConstraintStore *cs = constraintStoreOf( this );
  if ( cs )
  {
    if ( !cs->append( horizon, c.id, wkt, type, faciesCode, error ) )
      return false;

    QVariantList constraints = property( kConstraintsProp ).toList();
    QVariantMap rec = c.toMap();
    rec.insert( QStringLiteral( "horizon" ), horizon );
    rec.insert( QStringLiteral( "facies_code" ), faciesCode );
    constraints.append( rec );

    LayerDeclaration decl;
    decl.layerId = QStringLiteral( "constraints.%1" ).arg( horizon );
    decl.horizon = horizon;
    decl.type = QStringLiteral( "vector" );
    decl.source = QStringLiteral( "%1|layername=constraints|subset=horizon='%2'" )
                      .arg( cs->gpkgPath(), horizon );
    decl.group = QStringLiteral( "02_Constraints" );
    if ( !layers->declare( decl, error ) )
    {
      cs->remove( c.id );
      return false;
    }

    setProperty( kConstraintsProp, constraints );
    if ( constraintIdOut )
      *constraintIdOut = c.id;
    emit constraintAdded( c.id );
    return true;
  }

  // Fallback when no store is configured (preserves memory-layer compatibility)
  QVariantList constraints = property( kConstraintsProp ).toList();
  QVariantMap rec = c.toMap();
  rec.insert( QStringLiteral( "horizon" ), horizon );
  constraints.append( rec );

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
    constraints.removeLast();
    setProperty( kConstraintsProp, constraints );
    return false;
  }

  setProperty( kConstraintsProp, constraints );
  if ( constraintIdOut )
    *constraintIdOut = c.id;
  emit constraintAdded( c.id );
  return true;
}

QVector<QVariantMap> ConstraintWorkflow::loadConstraints( const QString &horizon )
{
  ConstraintStore *cs = constraintStoreOf( this );
  if ( !cs )
  {
    QVariantList list = property( kConstraintsProp ).toList();
    QVector<QVariantMap> res;
    for ( const QVariant &v : list )
    {
      const QVariantMap m = v.toMap();
      if ( horizon.isEmpty() || m.value( QStringLiteral( "horizon" ) ).toString() == horizon )
        res.append( m );
    }
    return res;
  }

  QVector<QVariantMap> loaded = cs->load( horizon );

  QVariantList constraints = property( kConstraintsProp ).toList();
  if ( horizon.isEmpty() )
  {
    constraints.clear();
  }
  else
  {
    for ( int i = constraints.size() - 1; i >= 0; --i )
    {
      if ( constraints.at( i ).toMap().value( QStringLiteral( "horizon" ) ).toString() == horizon )
        constraints.removeAt( i );
    }
  }

  int maxSeq = property( kSeqProp ).toInt();
  for ( const QVariantMap &rec : loaded )
  {
    constraints.append( rec );
    const QString id = rec.value( QStringLiteral( "id" ) ).toString();
    if ( id.startsWith( QStringLiteral( "c-" ) ) )
    {
      bool ok = false;
      int num = id.mid( 2 ).toInt( &ok );
      if ( ok && num > maxSeq )
        maxSeq = num;
    }
  }
  setProperty( kConstraintsProp, constraints );
  setProperty( kSeqProp, maxSeq );

  QgisLayerService *layers = layersOf( this );
  if ( layers && !loaded.isEmpty() )
  {
    if ( !horizon.isEmpty() )
    {
      LayerDeclaration decl;
      decl.layerId = QStringLiteral( "constraints.%1" ).arg( horizon );
      decl.horizon = horizon;
      decl.type = QStringLiteral( "vector" );
      decl.source = QStringLiteral( "%1|layername=constraints|subset=horizon='%2'" )
                        .arg( cs->gpkgPath(), horizon );
      decl.group = QStringLiteral( "02_Constraints" );
      layers->declare( decl );
    }
    else
    {
      QSet<QString> horizons;
      for ( const QVariantMap &rec : loaded )
      {
        const QString h = rec.value( QStringLiteral( "horizon" ) ).toString();
        if ( !h.isEmpty() )
          horizons.insert( h );
      }
      for ( const QString &h : horizons )
      {
        LayerDeclaration decl;
        decl.layerId = QStringLiteral( "constraints.%1" ).arg( h );
        decl.horizon = h;
        decl.type = QStringLiteral( "vector" );
        decl.source = QStringLiteral( "%1|layername=constraints|subset=horizon='%2'" )
                          .arg( cs->gpkgPath(), h );
        decl.group = QStringLiteral( "02_Constraints" );
        layers->declare( decl );
      }
    }
  }

  return loaded;
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
  if ( pointsLayerId.isEmpty() )
  {
    setError( error, tr( "没有井点图层" ) );
    return false;
  }
  if ( field.isEmpty() )
  {
    setError( error, tr( "插值字段为空" ) );
    return false;
  }
  if ( !( cellSize > 0.0 ) )
  {
    setError( error, tr( "像元大小必须是正数" ) );
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

  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
  {
    setError( error, manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );
    return false;
  }
  const QString constraintLayerId = QStringLiteral( "constraints.%1" ).arg( horizon );
  const bool hasConstraints = std::any_of(
      declared.cbegin(), declared.cend(),
      [&constraintLayerId]( const LayerDeclaration &d ) { return d.layerId == constraintLayerId; } );
  if ( hasConstraints )
  {
    QString constraintErr;
    QgsMapLayer *constraints = layers->instantiate( constraintLayerId, &constraintErr );
    if ( !constraints )
    {
      setError( error, constraintErr.isEmpty()
                           ? tr( "无法加载约束图层 %1" ).arg( constraintLayerId )
                           : constraintErr );
      return false;
    }
    params.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
  }

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

bool CompositionWorkflow::deriveFaciesPolygons( const QString &horizon, const QString &rasterLayerId,
                                               const QVariantMap &params, QString *error )
{
  const auto fail = [this, &horizon, error]( const QString &msg ) {
    setError( error, msg );
    emit faciesPolygonsFailed( horizon, msg );
    return false;
  };

  QgisProcessingService *proc = procOf( this );
  QgisLayerService *layers = layersOf( this );
  if ( !proc || !layers )
    return fail( tr( "composition workflow is not bound to services" ) );
  if ( rasterLayerId.isEmpty() )
    return fail( tr( "no raster layer supplied for facies polygons" ) );

  QVector<LayerDeclaration> declared;
  QString manifestErr;
  if ( !layers->tryDeclared( &declared, &manifestErr ) )
    return fail( manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr );

  QString declHorizon;
  bool found = false;
  for ( const LayerDeclaration &d : declared )
  {
    if ( d.layerId == rasterLayerId )
    {
      declHorizon = d.horizon;
      found = true;
      break;
    }
  }
  if ( !found )
    return fail( tr( "raster layer '%1' is not declared" ).arg( rasterLayerId ) );

  const QString h = !declHorizon.isEmpty() ? declHorizon : horizon;
  if ( h.isEmpty() )
    return fail( tr( "cannot derive facies polygons without a horizon" ) );

  QgsMapLayer *raster = layers->instantiate( rasterLayerId, error );
  if ( !raster )
  {
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "failed to instantiate raster '%1'" ).arg( rasterLayerId ) );
  }

  QVariantMap alg;
  alg.insert( QStringLiteral( "INPUT" ), QVariant::fromValue( raster ) );
  alg.insert( QStringLiteral( "OUTPUT" ), tempVectorPath( QStringLiteral( "facies" ), h ) );
  const QStringList forwarded = {
      QStringLiteral( "MIN_AREA" ), QStringLiteral( "SIMPLIFY" ),
      QStringLiteral( "SNAP_TOLERANCE" ), QStringLiteral( "ANGLE_TOLERANCE" ) };
  for ( const QString &key : forwarded )
  {
    if ( params.contains( key ) )
      alg.insert( key, params.value( key ) );
  }
  const QString constraintId = params.value( QStringLiteral( "CONSTRAINT_LAYER" ) ).toString();
  if ( !constraintId.isEmpty() )
  {
    QgsMapLayer *constraints = layers->instantiate( constraintId, error );
    if ( !constraints )
    {
      return fail( ( error && !error->isEmpty() )
                       ? *error
                       : tr( "failed to instantiate constraints '%1'" ).arg( constraintId ) );
    }
    alg.insert( QStringLiteral( "CONSTRAINTS" ), QVariant::fromValue( constraints ) );
  }

  const QVariantMap results = proc->run( QStringLiteral( "paleo:paleo_facies_polygonize" ), alg, error );
  if ( results.isEmpty() )
  {
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "facies polygonize produced no results" ) );
  }
  const QString outPath = outputPathOf( results );
  if ( outPath.isEmpty() )
    return fail( tr( "facies polygonize returned no output path" ) );

  LayerDeclaration decl;
  decl.layerId = QStringLiteral( "facies.%1" ).arg( h );
  decl.horizon = h;
  decl.type = QStringLiteral( "vector" );
  decl.source = QStringLiteral( "%1|layername=facies_polygons" ).arg( outPath );
  decl.group = QStringLiteral( "05_PaleoMap" );
  if ( !layers->declare( decl, error ) )
  {
    return fail( ( error && !error->isEmpty() )
                     ? *error
                     : tr( "failed to declare facies layer '%1'" ).arg( decl.layerId ) );
  }

  emit faciesPolygonsReady( h, decl.layerId );
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

void ValidationWorkflow::setProjectData( ProjectDataFacade *projectData )
{
  setProperty( "paleo.wf.projectdata", QVariant::fromValue( static_cast<QObject *>( projectData ) ) );
}

void ValidationWorkflow::setResidualThresholdMs( double thresholdMs )
{
  setProperty( "paleo.wf.residualThresholdMs", thresholdMs );
}

QVariantList ValidationWorkflow::lastResidualRows() const
{
  return property( "paleo.wf.residualRows" ).toList();
}

QList<ValidationIssue> ValidationWorkflow::validate()
{
  QList<ValidationIssue> issues;
  QgisLayerService *layers = layersOf( this );
  PaleoProjectStore *store = storeOf( this );
  QVector<LayerDeclaration> decls;
  if ( layers )
  {
    QString manifestErr;
    if ( !layers->tryDeclared( &decls, &manifestErr ) )
    {
      // Manifest unreadable is a finding, not a clean bill — otherwise a
      // corrupt store validates as "no issues".
      ValidationIssue v;
      v.severity = ValidationIssue::Error;
      v.code = QStringLiteral( "MANIFEST_READ_FAILED" );
      v.message = manifestErr.isEmpty() ? tr( "无法读取图层清单" ) : manifestErr;
      issues.append( v );
    }
  }

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

  // (d) 井上 D61 时间残差（autoplan §5C）— 只评 D61。每口井一行（验证页残差
  // 表渲染源，存 paleo.wf.residualRows）；|r|>阈值 → TIME_RESIDUAL 问题。
  // 默认阈值 10 ms；非数值行（无分层/TD 原因/不在测网内/落在空道）不成问题。
  {
    QVariantList rowMaps;
    if ( auto *pd = qobject_cast<ProjectDataFacade *>( property( "paleo.wf.projectdata" ).value<QObject *>() ) )
    {
      const double prop = property( "paleo.wf.residualThresholdMs" ).toDouble();
      const double threshold = prop > 0.0 ? prop : 10.0;
      const QList<TimeResidualRow> rows =
          computeTimeResiduals( pd, QStringLiteral( "D61" ), threshold );
      // 残差行所属栅格图层（问题行的地图缩放目标）。
      const QString rasterLayerId =
          pd->horizonRasterDecl( QStringLiteral( "D61" ) ).layerId;
      for ( const TimeResidualRow &row : rows )
      {
        QVariantMap m;
        m.insert( QStringLiteral( "well_id" ), row.wellId );
        m.insert( QStringLiteral( "well_name" ), row.wellName );
        m.insert( QStringLiteral( "threshold_ms" ), threshold );
        m.insert( QStringLiteral( "reason" ), row.reason );
        if ( std::isfinite( row.x ) )
          m.insert( QStringLiteral( "x" ), row.x );
        if ( std::isfinite( row.y ) )
          m.insert( QStringLiteral( "y" ), row.y );
        if ( std::isfinite( row.timeMs ) )
          m.insert( QStringLiteral( "time_ms" ), row.timeMs );
        if ( std::isfinite( row.rasterMs ) )
          m.insert( QStringLiteral( "raster_ms" ), row.rasterMs );
        if ( std::isfinite( row.residualMs ) )
          m.insert( QStringLiteral( "residual_ms" ), row.residualMs );
        m.insert( QStringLiteral( "inline" ), row.inlineNo );
        const char *status = row.status == TimeResidualRow::Status::Pass       ? "pass"
                             : row.status == TimeResidualRow::Status::Exceeds  ? "exceed"
                             : row.status == TimeResidualRow::Status::Warn     ? "warn"
                                                                               : "na";
        m.insert( QStringLiteral( "status" ), QString::fromLatin1( status ) );
        rowMaps.append( m );

        if ( row.status != TimeResidualRow::Status::Exceeds )
          continue;
        ValidationIssue v;
        v.severity = ValidationIssue::Warning;
        v.code = QStringLiteral( "TIME_RESIDUAL" );
        QString inlineText;
        if ( row.inlineNo >= 0 )
          inlineText = tr( "，目标测线 %1" ).arg( row.inlineNo );
        v.message = tr( "井 %1 %2 时间残差 %3ms（井 %4ms vs 栅格 %5ms%6）" )
                        .arg( row.wellName, QStringLiteral( "D61" ) )
                        .arg( row.residualMs, 0, 'f', 1 )
                        .arg( row.timeMs, 0, 'f', 1 )
                        .arg( row.rasterMs, 0, 'f', 1 )
                        .arg( inlineText );
        v.horizon = QStringLiteral( "D61" );
        v.wellId = row.wellId;
        if ( std::isfinite( row.x ) && std::isfinite( row.y ) )
          v.wktLocation = QStringLiteral( "POINT(%1 %2)" ).arg( row.x ).arg( row.y );
        v.details.insert( QStringLiteral( "well_name" ), row.wellName );
        v.details.insert( QStringLiteral( "well_x" ), row.x );
        v.details.insert( QStringLiteral( "well_y" ), row.y );
        v.details.insert( QStringLiteral( "inline" ), row.inlineNo );
        v.details.insert( QStringLiteral( "time_ms" ), row.timeMs );
        v.details.insert( QStringLiteral( "raster_ms" ), row.rasterMs );
        v.details.insert( QStringLiteral( "residual_ms" ), row.residualMs );
        v.layerId = rasterLayerId;
        issues.append( v );
      }
    }
    setProperty( "paleo.wf.residualRows", rowMaps );
  }

  emit validationDone( issues.size() );
  return issues;
}
