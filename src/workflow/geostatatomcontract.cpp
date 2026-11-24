// 层：功能
// workflow/geostatatomcontract.cpp — 见 geostatatomcontract.h。
#include "geostatatomcontract.h"

#include "capabilitygateway.h"
#include "services/geostaticapabilityspec.h"

#include <QJsonArray>
#include <QJsonValue>

#include <cmath>
#include <limits>

namespace paleo::geostat_atom
{
namespace
{

bool numericVariant( const QVariant &value )
{
  switch( value.userType() )
  {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
      return true;
    default:
      return false;
  }
}

bool finiteNumber( const QVariant &value, double *number )
{
  // JSON 数值经 QJsonValue::toVariant 可能是 Int/LongLong（整值表示），
  // C++ 调用侧是 Double——数值家族都接受，只拒非有限值与非数值类型。
  if( !numericVariant( value ) )
    return false;
  const double result = value.toDouble();
  if( !std::isfinite( result ) )
    return false;
  *number = result;
  return true;
}

// 引用/身份字符串：非空、长度受限、无控制字符。ID 可含非 ASCII（层键、
// 方案/单元 id 由工程数据决定），不做字符集猜测。
bool token( const QVariant &value, int maximum )
{
  if( value.userType() != QMetaType::QString )
    return false;
  const QString text = value.toString();
  if( text.isEmpty() || text.size() > maximum )
    return false;
  for( const auto &ch : text )
    if( ch.unicode() < 0x20 || ch.unicode() == 0x7f )
      return false;
  return true;
}

bool hexDigest( const QString &text )
{
  if( text.size() != 64 )
    return false;
  for( const auto &ch : text )
    if( !( ( ch >= '0' && ch <= '9' ) || ( ch >= 'a' && ch <= 'f' ) ) )
      return false;
  return true;
}

bool exactInteger( const QJsonValue &value, int *out )
{
  if( !value.isDouble() || !std::isfinite( value.toDouble() ) ||
      std::floor( value.toDouble() ) != value.toDouble() ||
      value.toDouble() < double( std::numeric_limits<int>::min() ) ||
      value.toDouble() > double( std::numeric_limits<int>::max() ) )
    return false;
  *out = value.toInt();
  return true;
}

bool multiplyBytes( qint64 a, qint64 b, qint64 *out )
{
  if( a < 0 || b < 0 || ( a != 0 && b > std::numeric_limits<qint64>::max() / a ) )
    return false;
  *out = a * b;
  return true;
}

bool addBytes( qint64 a, qint64 b, qint64 *out )
{
  if( a < 0 || b < 0 || a > std::numeric_limits<qint64>::max() - b )
    return false;
  *out = a + b;
  return true;
}

QString decodeParameters( GeostatMethod method, const QVariantMap &parameters,
                          GeostatDecodedRequest *decoded )
{
  // 首版明确不支持个体实现、屏障、协变量；不能只按未知键含糊带过。
  static const QStringList kUnsupported{
      QStringLiteral( "MEMBERS" ),          QStringLiteral( "PERSIST_REALIZATIONS" ),
      QStringLiteral( "MEMBER_PATH_PREFIX" ), QStringLiteral( "COVARIATE" ),
      QStringLiteral( "COVARIATE_REF" ),    QStringLiteral( "CONSTRAINTS" ),
      QStringLiteral( "CONSTRAINTS_REF" ),  QStringLiteral( "BARRIERS" ),
      QStringLiteral( "IGNORE_BARRIERS" ) };
  for( auto it = parameters.constBegin(); it != parameters.constEnd(); ++it )
    if( kUnsupported.contains( it.key() ) )
      return QStringLiteral( "geostat_param_unsupported" );
  // 参数：先过 AlgorithmSpec 单一来源（默认值 + 域），再做本契约的收紧。
  //（ValidatedParams.code 成功时为 "ok"，失败码只在空 effective 表上生效。）
  QString paramCode;
  decoded->effectiveParams =
      paleo::capability::effectiveScalarParams( algorithmIdForMethod( method ),
                                                kGeostatAtomAlgorithmContractVersion, parameters,
                                                &paramCode );
  if( decoded->effectiveParams.isEmpty() )
    return QStringLiteral( "geostat_param_" ) +
           ( paramCode.isEmpty() || paramCode == QLatin1String( "ok" ) ? QStringLiteral( "invalid" )
                                                                        : paramCode );
  const AlgorithmSpec spec = AlgorithmSpecRegistry::specAtVersion(
      algorithmIdForMethod( method ), kGeostatAtomAlgorithmContractVersion );
  // 显式给出的参数必须是严格数值表示：validateAndFillParams 的 toInt() 会
  // 截断分数——「校验通过但解码成另一值」在此收紧拒绝。
  for( auto it = parameters.constBegin(); it != parameters.constEnd(); ++it )
  {
    const QVariant &value = it.value();
    if( it.key() == QLatin1String( "SEED" ) )
      continue; // SGS 下方单独精确解析
    if( numericVariant( value ) )
    {
      const double number = value.toDouble();
      if( !std::isfinite( number ) )
        return QStringLiteral( "geostat_param_not_finite" );
      bool integerField = false;
      for( const auto &field : spec.params )
        if( field.key == it.key() && field.type == AlgorithmParamField::Int )
          integerField = true;
      if( integerField && std::floor( number ) != number )
        return QStringLiteral( "geostat_param_not_integer" );
    }
    else if( value.userType() != QMetaType::QString )
    {
      return QStringLiteral( "geostat_param_type_mismatch" );
    }
  }
  // 变差模型（有效参数 → 冻结值；域在本契约收紧：sill/range 严格 > 0）。
  const QVariantMap &effective = decoded->effectiveParams;
  decoded->variogram.model = effective.value( QStringLiteral( "VARIOGRAM_MODEL" ) ).toString();
  if( !variogramModelVocabulary().contains( decoded->variogram.model ) )
    return QStringLiteral( "geostat_variogram_model_invalid" );
  decoded->variogram.nugget = effective.value( QStringLiteral( "NUGGET" ) ).toDouble();
  decoded->variogram.sill = effective.value( QStringLiteral( "SILL" ) ).toDouble();
  decoded->variogram.range = effective.value( QStringLiteral( "RANGE" ) ).toDouble();
  decoded->variogram.anisotropyRatio =
      effective.value( QStringLiteral( "ANISOTROPY_RATIO" ) ).toDouble();
  decoded->variogram.azimuthDeg = effective.value( QStringLiteral( "AZIMUTH_DEG" ) ).toDouble();
  if( !decoded->variogram.valid() )
    return QStringLiteral( "geostat_variogram_out_of_range" );
  if( method == GeostatMethod::Sgs )
  {
    if( !parseGeostatSeed( effective.value( QStringLiteral( "SEED" ) ), &decoded->seed ) )
      return QStringLiteral( "geostat_param_seed_invalid" );
    const int realizations = effective.value( QStringLiteral( "REALIZATIONS" ) ).toInt();
    if( realizations < 1 || realizations > 64 )
      return QStringLiteral( "geostat_param_out_of_range" );
  }
  return {};
}

QString decodeGrid( const QVariant &gridValue, GeostatGridPlan *grid )
{
  if( gridValue.userType() != QMetaType::QVariantMap )
    return QStringLiteral( "geostat_grid_missing" );
  const QVariantMap map = gridValue.toMap();
  if( map.size() != 5 )
    return QStringLiteral( "geostat_grid_unknown_field" );
  double minX = 0, maxX = 0, minY = 0, maxY = 0, cellSize = 0;
  if( !finiteNumber( map.value( QStringLiteral( "min_x" ) ), &minX ) ||
      !finiteNumber( map.value( QStringLiteral( "max_x" ) ), &maxX ) ||
      !finiteNumber( map.value( QStringLiteral( "min_y" ) ), &minY ) ||
      !finiteNumber( map.value( QStringLiteral( "max_y" ) ), &maxY ) ||
      !finiteNumber( map.value( QStringLiteral( "cell_size" ) ), &cellSize ) )
    return QStringLiteral( "geostat_grid_not_finite" );
  if( !( cellSize > 0 ) )
    return QStringLiteral( "geostat_grid_cell_size_invalid" );
  if( !( maxX > minX ) || !( maxY > minY ) )
    return QStringLiteral( "geostat_grid_extent_degenerate" );
  // checked arithmetic：跨度/维数先验有限与域，再做整数乘法。
  const double spanX = maxX - minX;
  const double spanY = maxY - minY;
  if( !std::isfinite( spanX ) || !std::isfinite( spanY ) )
    return QStringLiteral( "geostat_grid_extent_overflow" );
  const double cols = std::ceil( spanX / cellSize );
  const double rows = std::ceil( spanY / cellSize );
  if( !std::isfinite( cols ) || !std::isfinite( rows ) || cols < 1 || rows < 1 )
    return QStringLiteral( "geostat_grid_dimension_overflow" );
  if( cols > kGeostatAtomMaxGridSide || rows > kGeostatAtomMaxGridSide )
    return QStringLiteral( "geostat_grid_side_exceeded" );
  grid->cols = static_cast<int>( cols );
  grid->rows = static_cast<int>( rows );
  if( qint64( grid->cols ) * grid->rows > kGeostatAtomMaxCells )
    return QStringLiteral( "geostat_grid_cell_budget_exceeded" );
  // north-up：origin = 左上像素外角；pixelHeight < 0。
  grid->originX = minX;
  grid->originY = maxY;
  grid->pixelWidth = cellSize;
  grid->pixelHeight = -cellSize;
  return {};
}

QString decodeSampleIdentity( const QVariantMap &inputs, GeostatDecodedRequest *decoded )
{
  const QVariant sampleRef = inputs.value( QStringLiteral( "sample_ref" ) );
  if( !token( sampleRef, 256 ) )
    return QStringLiteral( "geostat_input_sample_ref_invalid" );
  decoded->sampleRef = sampleRef.toString();
  // 样本因子：单位与字段取自单因素词表（已有已确认契约），不猜字段名。
  const QVariant factor = inputs.value( QStringLiteral( "factor_id" ) );
  QString code;
  if( !token( factor, 64 ) ||
      !resolveGeostatFactor( factor.toString(), &decoded->factorUnit, &decoded->factorField, &code ) )
    return code.isEmpty() ? QStringLiteral( "geostat_factor_unsupported" ) : code;
  decoded->factorId = factor.toString();
  if( inputs.contains( QStringLiteral( "field" ) ) )
  {
    const QVariant field = inputs.value( QStringLiteral( "field" ) );
    if( !token( field, 128 ) )
      return QStringLiteral( "geostat_input_field_invalid" );
    if( field.toString() != decoded->factorField )
      return QStringLiteral( "geostat_field_mismatch" );
  }
  // unit_selection：形状校验（实体核验在宿主 capture 侧做 DS 校验）。
  const QVariant unit = inputs.value( QStringLiteral( "unit_selection" ) );
  if( unit.userType() != QMetaType::QVariantMap )
    return QStringLiteral( "geostat_input_unit_invalid" );
  decoded->unitSelection = unit.toMap();
  // 嵌套键封闭（与工具 schema 的 additionalProperties:false 同义）。
  static const QStringList kUnitKeys{ QStringLiteral( "schemeId" ), QStringLiteral( "revision" ),
                                      QStringLiteral( "intervalId" ), QStringLiteral( "subIntervalId" ),
                                      QStringLiteral( "aggregate" ) };
  for( auto it = decoded->unitSelection.cbegin(); it != decoded->unitSelection.cend(); ++it )
    if( !kUnitKeys.contains( it.key() ) )
      return QStringLiteral( "geostat_input_unit_unknown_field" );
  for( const auto &key : { QStringLiteral( "schemeId" ), QStringLiteral( "intervalId" ),
                           QStringLiteral( "subIntervalId" ) } )
    if( !token( decoded->unitSelection.value( key ), 128 ) )
      return QStringLiteral( "geostat_input_unit_incomplete" );
  if( decoded->unitSelection.value( QStringLiteral( "aggregate" ) ).toBool() )
    return QStringLiteral( "geostat_input_unit_incomplete" );
  if( decoded->unitSelection.contains( QStringLiteral( "revision" ) ) &&
      !token( decoded->unitSelection.value( QStringLiteral( "revision" ) ), 128 ) )
    return QStringLiteral( "geostat_input_unit_incomplete" );
  return {};
}

} // namespace

GeostatMethod methodForCapability( const QString &capabilityId, bool *known )
{
  if( capabilityId == kKrigingCapabilityId() )
  {
    if( known )
      *known = true;
    return GeostatMethod::Kriging;
  }
  if( capabilityId == kSgsCapabilityId() )
  {
    if( known )
      *known = true;
    return GeostatMethod::Sgs;
  }
  if( known )
    *known = false;
  return GeostatMethod::Kriging;
}

QString algorithmIdForMethod( GeostatMethod method )
{
  return method == GeostatMethod::Sgs ? kSgsAlgorithmId() : kKrigingAlgorithmId();
}

QString capabilityIdForMethod( GeostatMethod method )
{
  return method == GeostatMethod::Sgs ? kSgsCapabilityId() : kKrigingCapabilityId();
}

bool parseGeostatSeed( const QVariant &value, quint64 *seed )
{
  if( seed )
    *seed = 0;
  if( value.userType() == QMetaType::QString )
  {
    const QString text = value.toString();
    if( text.isEmpty() || text.size() > 20 )
      return false;
    quint64 parsed = 0;
    for( const auto &ch : text )
    {
      if( ch < '0' || ch > '9' )
        return false;
      const quint64 digit = quint64( ch.unicode() - '0' );
      if( parsed > ( std::numeric_limits<quint64>::max() - digit ) / 10 )
        return false;
      parsed = parsed * 10 + digit;
    }
    if( seed )
      *seed = parsed;
    return true;
  }
  // JSON 原生整值：仅在 double 可精确表示的范围内接受（≤2^53）。
  double number = 0;
  if( !finiteNumber( value, &number ) || std::floor( number ) != number || number < 0 ||
      number > 9007199254740992.0 )
    return false;
  if( seed )
    *seed = static_cast<quint64>( number );
  return true;
}

QJsonObject GeostatGridPlan::toJson() const
{
  return QJsonObject{ { "cols", cols },
                      { "rows", rows },
                      { "origin_x", originX },
                      { "origin_y", originY },
                      { "pixel_width", pixelWidth },
                      { "pixel_height", pixelHeight } };
}

bool GeostatGridPlan::valid() const
{
  return cols > 0 && rows > 0 && pixelWidth > 0 && pixelHeight < 0 && std::isfinite( originX ) &&
         std::isfinite( originY ) && std::isfinite( pixelWidth ) && std::isfinite( pixelHeight ) &&
         qint64( cols ) * rows <= kGeostatAtomMaxCells;
}

GeostatGridPlan GeostatGridPlan::fromJson( const QJsonObject &json, bool *ok )
{
  GeostatGridPlan plan;
  const bool shape = exactInteger( json.value( "cols" ), &plan.cols ) &&
                     exactInteger( json.value( "rows" ), &plan.rows ) &&
                     json.value( "origin_x" ).isDouble() && json.value( "origin_y" ).isDouble() &&
                     json.value( "pixel_width" ).isDouble() && json.value( "pixel_height" ).isDouble();
  if( shape )
  {
    plan.originX = json.value( "origin_x" ).toDouble();
    plan.originY = json.value( "origin_y" ).toDouble();
    plan.pixelWidth = json.value( "pixel_width" ).toDouble();
    plan.pixelHeight = json.value( "pixel_height" ).toDouble();
  }
  if( ok )
    *ok = shape && plan.valid();
  return shape ? plan : GeostatGridPlan{};
}

QJsonObject GeostatVariogramPlan::toJson() const
{
  return QJsonObject{ { "model", model },           { "nugget", nugget },       { "sill", sill },
                      { "range", range },           { "anisotropy_ratio", anisotropyRatio },
                      { "azimuth_deg", azimuthDeg } };
}

bool GeostatVariogramPlan::valid() const
{
  return variogramModelVocabulary().contains( model ) && std::isfinite( nugget ) &&
         std::isfinite( sill ) && std::isfinite( range ) && std::isfinite( anisotropyRatio ) &&
         std::isfinite( azimuthDeg ) && nugget >= 0 && sill > 0 && range > 0 &&
         anisotropyRatio >= 1 && azimuthDeg >= 0 && azimuthDeg < 360;
}

GeostatVariogramPlan GeostatVariogramPlan::fromJson( const QJsonObject &json, bool *ok )
{
  GeostatVariogramPlan plan;
  plan.model = json.value( "model" ).toString();
  plan.nugget = json.value( "nugget" ).toDouble();
  plan.sill = json.value( "sill" ).toDouble();
  plan.range = json.value( "range" ).toDouble();
  plan.anisotropyRatio = json.value( "anisotropy_ratio" ).toDouble();
  plan.azimuthDeg = json.value( "azimuth_deg" ).toDouble();
  const bool validPlan = json.value( "model" ).isString() && json.value( "nugget" ).isDouble() &&
                         json.value( "sill" ).isDouble() && json.value( "range" ).isDouble() &&
                         json.value( "anisotropy_ratio" ).isDouble() &&
                         json.value( "azimuth_deg" ).isDouble() && plan.valid();
  if( ok )
    *ok = validPlan;
  return validPlan ? plan : GeostatVariogramPlan{};
}

GeostatResourceEstimate estimateGeostatResources( GeostatMethod method,
                                                  const QVariantMap &effectiveParams, int cols,
                                                  int rows, int sampleCount, bool *ok )
{
  GeostatResourceEstimate estimate;
  const auto done = [ok]( bool value, const GeostatResourceEstimate &result ) {
    if( ok )
      *ok = value;
    return value ? result : GeostatResourceEstimate{};
  };
  if( cols <= 0 || rows <= 0 || sampleCount <= 0 || sampleCount > kGeostatAtomMaxSamples )
    return done( false, estimate );
  const qint64 cells = qint64( cols ) * rows;
  const int realizations =
      method == GeostatMethod::Sgs ? effectiveParams.value( QStringLiteral( "REALIZATIONS" ) ).toInt() : 0;
  if( method == GeostatMethod::Sgs && ( realizations < 1 || realizations > 64 ) )
    return done( false, estimate );
  // 字节口径（估算，非 RSS 实测）：
  //   样本 Sample{x,y,value}=24B；邻域索引/条件点最坏每样本 48B；
  //   工作数组：估值+方差（克里金）或均值+离散（SGS），各 cells×8B；
  //   SGS 实现数组：R×cells×8B（与内核 kMaxRealizationBytes 同口径）；
  //   逐格簿记 cells×8B；K×K（K≤64）稠密系统与工作向量 4 份；
  //   快照 JSON 每样本 ≤80B + 头；两个 Float32 端口栅格 cells×4B + 头余量。
  qint64 sampleArray = 0, sampleIndex = 0, grid = 0, realizationsBytes = 0, scratchCells = 0;
  qint64 snapshotRows = 0, output = 0;
  const qint64 scratchSolver = 64LL * 64 * 8 * 4;
  if( !multiplyBytes( sampleCount, 24, &sampleArray ) || !multiplyBytes( sampleCount, 48, &sampleIndex ) ||
      !multiplyBytes( cells, 16, &grid ) || !multiplyBytes( cells, 8, &scratchCells ) ||
      !multiplyBytes( sampleCount, 80, &snapshotRows ) || !multiplyBytes( cells, 8, &output ) )
    return done( false, estimate );
  if( method == GeostatMethod::Sgs && !multiplyBytes( cells, qint64( realizations ) * 8, &realizationsBytes ) )
    return done( false, estimate );
  estimate.sampleBytes = sampleArray + sampleIndex;
  estimate.gridBytes = grid;
  estimate.realizationBytes = realizationsBytes;
  estimate.scratchBytes = scratchCells + scratchSolver;
  estimate.snapshotBytes = snapshotRows + 4096;
  estimate.outputBytes = output + 2 * 65536;
  if( !addBytes( estimate.sampleBytes, estimate.gridBytes, &estimate.peakBytes ) ||
      !addBytes( estimate.peakBytes, estimate.realizationBytes, &estimate.peakBytes ) ||
      !addBytes( estimate.peakBytes, estimate.scratchBytes, &estimate.peakBytes ) )
    return done( false, estimate );
  return done( true, estimate );
}

QString geostatEstimateAdmission( const GeostatResourceEstimate &estimate )
{
  if( estimate.realizationBytes > kGeostatAtomMaxRealizationBytes )
    return QStringLiteral( "geostat_realization_budget_exceeded" );
  if( estimate.peakBytes > kGeostatAtomMaxPeakBytes )
    return QStringLiteral( "geostat_peak_budget_exceeded" );
  if( estimate.snapshotBytes > kGeostatAtomMaxSnapshotBytes )
    return QStringLiteral( "geostat_snapshot_budget_exceeded" );
  if( estimate.outputBytes > kGeostatAtomMaxOutputBytes )
    return QStringLiteral( "geostat_output_budget_exceeded" );
  return {};
}

QJsonObject GeostatResourceEstimate::toJson() const
{
  return QJsonObject{ { "sample_bytes", double( sampleBytes ) },
                      { "grid_bytes", double( gridBytes ) },
                      { "realization_bytes", double( realizationBytes ) },
                      { "scratch_bytes", double( scratchBytes ) },
                      { "peak_bytes", double( peakBytes ) },
                      { "snapshot_bytes", double( snapshotBytes ) },
                      { "output_bytes", double( outputBytes ) } };
}

bool GeostatResourceEstimate::fromJson( const QJsonObject &json, GeostatResourceEstimate *out )
{
  const auto read = [&json]( const char *key, qint64 *value ) {
    const QJsonValue field = json.value( QLatin1String( key ) );
    if( !field.isDouble() || !std::isfinite( field.toDouble() ) || field.toDouble() < 0 ||
        std::floor( field.toDouble() ) != field.toDouble() ||
        field.toDouble() > 9007199254740992.0 )
      return false;
    *value = qint64( field.toDouble() );
    return true;
  };
  GeostatResourceEstimate estimate;
  if( !read( "sample_bytes", &estimate.sampleBytes ) || !read( "grid_bytes", &estimate.gridBytes ) ||
      !read( "realization_bytes", &estimate.realizationBytes ) ||
      !read( "scratch_bytes", &estimate.scratchBytes ) || !read( "peak_bytes", &estimate.peakBytes ) ||
      !read( "snapshot_bytes", &estimate.snapshotBytes ) ||
      !read( "output_bytes", &estimate.outputBytes ) )
    return false;
  *out = estimate;
  return true;
}

QString geostatSnapshotRelativePath( const QString &snapshotSha256 )
{
  return QStringLiteral( ".paleo/capability-ops/geostat-snapshots/" ) + snapshotSha256 +
         QStringLiteral( "/samples.json" );
}

QJsonObject GeostatSamplePlan::toJson() const
{
  return QJsonObject{ { "version_id", versionId },
                      { "source_sha256", sourceSha256 },
                      { "layer_key", layerKey },
                      { "association", association },
                      { "factor_id", factorId },
                      { "field", field },
                      { "value_unit", valueUnit },
                      { "crs_authid", crsAuthId },
                      { "crs_wkt_sha256", crsWktSha256 },
                      { "snapshot_path", snapshotPath },
                      { "snapshot_sha256", snapshotSha256 },
                      { "sample_count", sampleCount },
                      { "skipped_rows", skippedRows } };
}

bool GeostatSamplePlan::valid() const
{
  return !versionId.isEmpty() && !sourceSha256.isEmpty() && !factorId.isEmpty() && !field.isEmpty() &&
         !valueUnit.isEmpty() && hexDigest( snapshotSha256 ) && hexDigest( crsWktSha256 ) &&
         !snapshotPath.isEmpty() && sampleCount > 0 && sampleCount <= kGeostatAtomMaxSamples &&
         skippedRows >= 0 &&
         ( association == QLatin1String( "layer_key" ) || association == QLatin1String( "version_unit" ) );
}

GeostatSamplePlan GeostatSamplePlan::fromJson( const QJsonObject &json, bool *ok )
{
  GeostatSamplePlan plan;
  plan.versionId = json.value( "version_id" ).toString();
  plan.sourceSha256 = json.value( "source_sha256" ).toString();
  plan.layerKey = json.value( "layer_key" ).toString();
  plan.association = json.value( "association" ).toString();
  plan.factorId = json.value( "factor_id" ).toString();
  plan.field = json.value( "field" ).toString();
  plan.valueUnit = json.value( "value_unit" ).toString();
  plan.crsAuthId = json.value( "crs_authid" ).toString();
  plan.crsWktSha256 = json.value( "crs_wkt_sha256" ).toString();
  plan.snapshotPath = json.value( "snapshot_path" ).toString();
  plan.snapshotSha256 = json.value( "snapshot_sha256" ).toString();
  const bool counts = exactInteger( json.value( "sample_count" ), &plan.sampleCount ) &&
                      exactInteger( json.value( "skipped_rows" ), &plan.skippedRows );
  const bool validPlan = counts && plan.valid();
  if( ok )
    *ok = validPlan;
  return validPlan ? plan : GeostatSamplePlan{};
}

QJsonObject GeostatFrozenPlan::toJson() const
{
  QJsonObject json;
  json.insert( "schema", schema );
  json.insert( "method", method == GeostatMethod::Sgs ? QStringLiteral( "sgs" )
                                                      : QStringLiteral( "kriging" ) );
  json.insert( "effective_parameters", QJsonObject::fromVariantMap( effectiveParams ) );
  json.insert( "variogram", variogram.toJson() );
  json.insert( "grid", grid.toJson() );
  json.insert( "unit", QJsonObject::fromVariantMap( unit ) );
  json.insert( "sample", sample.toJson() );
  json.insert( "resource_estimate", estimate.toJson() );
  if( method == GeostatMethod::Sgs )
    json.insert( "seed", QString::number( seed ) ); // 十进制字符串，精确往返
  // operation retention 已登记的引用形态：父目录（= 快照目录）成为 GC 根。
  json.insert( "sourceVerification",
               QJsonObject{ { "path", sample.snapshotPath }, { "versionId", sample.versionId } } );
  return json;
}

GeostatFrozenPlan GeostatFrozenPlan::fromJson( const QJsonObject &json, bool *ok )
{
  const auto fail = [ok]() {
    if( ok )
      *ok = false;
    return GeostatFrozenPlan{};
  };
  GeostatFrozenPlan plan;
  if( json.value( "schema" ).toString() != plan.schema || !json.value( "effective_parameters" ).isObject() ||
      !json.value( "unit" ).isObject() )
    return fail();
  const QString method = json.value( "method" ).toString();
  if( method == QLatin1String( "kriging" ) )
    plan.method = GeostatMethod::Kriging;
  else if( method == QLatin1String( "sgs" ) )
    plan.method = GeostatMethod::Sgs;
  else
    return fail();
  plan.effectiveParams = json.value( "effective_parameters" ).toObject().toVariantMap();
  bool part = false;
  plan.variogram = GeostatVariogramPlan::fromJson( json.value( "variogram" ).toObject(), &part );
  if( !part )
    return fail();
  plan.grid = GeostatGridPlan::fromJson( json.value( "grid" ).toObject(), &part );
  if( !part )
    return fail();
  plan.sample = GeostatSamplePlan::fromJson( json.value( "sample" ).toObject(), &part );
  if( !part )
    return fail();
  if( !GeostatResourceEstimate::fromJson( json.value( "resource_estimate" ).toObject(), &plan.estimate ) )
    return fail();
  plan.unit = json.value( "unit" ).toObject().toVariantMap();
  if( plan.method == GeostatMethod::Sgs &&
      !parseGeostatSeed( json.value( "seed" ).toVariant(), &plan.seed ) )
    return fail();
  // retention 引用必须与 sample.* 同值（否则计划不可信）。
  const QJsonObject verification = json.value( "sourceVerification" ).toObject();
  if( verification.value( "path" ).toString() != plan.sample.snapshotPath ||
      verification.value( "versionId" ).toString() != plan.sample.versionId )
    return fail();
  if( ok )
    *ok = true;
  return plan;
}

QString validateGeostatFrozenPlan( const GeostatFrozenPlan &plan )
{
  if( plan.schema != QStringLiteral( "paleo.geostat-atom.v1" ) )
    return QStringLiteral( "geostat_plan_schema_unsupported" );
  if( !variogramModelVocabulary().contains( plan.variogram.model ) )
    return QStringLiteral( "geostat_variogram_model_invalid" );
  if( !plan.variogram.valid() )
    return QStringLiteral( "geostat_variogram_out_of_range" );
  if( !plan.grid.valid() )
    return QStringLiteral( "geostat_grid_invalid" );
  // 冻结单元身份（S1–S4 子层段；aggregate 拒绝）。
  if( plan.unit.value( QStringLiteral( "schemeId" ) ).toString().isEmpty() ||
      plan.unit.value( QStringLiteral( "revision" ) ).toString().isEmpty() ||
      plan.unit.value( QStringLiteral( "intervalId" ) ).toString().isEmpty() ||
      plan.unit.value( QStringLiteral( "subIntervalId" ) ).toString().isEmpty() ||
      plan.unit.value( QStringLiteral( "aggregate" ) ).toBool() )
    return QStringLiteral( "geostat_unit_invalid" );
  // 有效参数与其投影必须同源且逐项一致：不能只分别校验范围，否则被改的
  // RANGE/SEED 可在同一计划里互相矛盾。
  GeostatDecodedRequest parameters;
  if( const QString code = decodeParameters( plan.method, plan.effectiveParams, &parameters );
      !code.isEmpty() )
    return code;
  if( QJsonObject::fromVariantMap( parameters.effectiveParams ) !=
          QJsonObject::fromVariantMap( plan.effectiveParams ) ||
      !( parameters.variogram == plan.variogram ) ||
      ( plan.method == GeostatMethod::Sgs && parameters.seed != plan.seed ) )
    return QStringLiteral( "geostat_plan_parameters_mismatch" );
  if( !plan.sample.valid() || plan.sample.snapshotPath != geostatSnapshotRelativePath( plan.sample.snapshotSha256 ) )
    return QStringLiteral( "geostat_sample_plan_invalid" );
  bool ok = false;
  const GeostatResourceEstimate expected =
      estimateGeostatResources( plan.method, plan.effectiveParams, plan.grid.cols, plan.grid.rows,
                                plan.sample.sampleCount, &ok );
  if( !ok )
    return QStringLiteral( "geostat_sample_plan_invalid" );
  if( !( expected == plan.estimate ) )
    return QStringLiteral( "geostat_resource_estimate_mismatch" );
  if( const QString code = geostatEstimateAdmission( plan.estimate ); !code.isEmpty() )
    return code;
  return {};
}

QString decodeGeostatAtomRequest( GeostatMethod method, const QVariantMap &parameters,
                                  const QVariantMap &inputs, const QString &publishIntent,
                                  GeostatDecodedRequest *out )
{
  if( out )
    *out = GeostatDecodedRequest{};
  // publish 只经既有 source_operation_id 发布链（CapabilityHost::prepare 的
  // freezeSourcePublication 分支）；能到达本解码的 publish 一律缺源操作。
  const QString intent = publishIntent.isEmpty() ? QStringLiteral( "compute-only" ) : publishIntent;
  if( intent == QLatin1String( "publish" ) )
    return QStringLiteral( "publish_requires_source_operation" );
  if( intent != QLatin1String( "prepare" ) && intent != QLatin1String( "compute-only" ) )
    return QStringLiteral( "unsupported_intent" );
  GeostatDecodedRequest decoded;
  decoded.method = method;
  if( const QString code = decodeParameters( method, parameters, &decoded ); !code.isEmpty() )
    return code;

  // 输入：封闭键集合。首版明确拒绝 constraints_ref / 协变量 / 屏障 / 任意
  // 样本数组——不静默忽略。
  static const QStringList kAllowedInputs{ QStringLiteral( "sample_ref" ),
                                           QStringLiteral( "factor_id" ),
                                           QStringLiteral( "field" ),
                                           QStringLiteral( "unit_selection" ),
                                           QStringLiteral( "grid" ),
                                           QStringLiteral( "source_operation_id" ) };
  static const QStringList kUnsupportedInputs{ QStringLiteral( "constraints_ref" ),
                                               QStringLiteral( "covariate_ref" ),
                                               QStringLiteral( "barriers" ),
                                               QStringLiteral( "samples" ),
                                               QStringLiteral( "members" ),
                                               QStringLiteral( "MEMBERS" ),
                                               QStringLiteral( "persist_realizations" ),
                                               QStringLiteral( "persistRealizations" ),
                                               QStringLiteral( "member_path_prefix" ),
                                               QStringLiteral( "ignore_barriers" ) };
  for( auto it = inputs.constBegin(); it != inputs.constEnd(); ++it )
  {
    if( kUnsupportedInputs.contains( it.key() ) )
      return QStringLiteral( "geostat_input_unsupported" );
    if( !kAllowedInputs.contains( it.key() ) )
      return QStringLiteral( "geostat_input_unknown" );
  }

  // compute-only 继承：只接受 source_operation_id 一个输入；冻结计划（含
  // 参数）由持久 prepare 记录继承，不重新取样本。
  if( inputs.contains( QStringLiteral( "source_operation_id" ) ) )
  {
    const QVariant sourceOperation = inputs.value( QStringLiteral( "source_operation_id" ) );
    if( intent != QLatin1String( "compute-only" ) )
      return QStringLiteral( "geostat_input_source_operation_misplaced" );
    if( !token( sourceOperation, 128 ) )
      return QStringLiteral( "geostat_input_source_operation_invalid" );
    if( inputs.size() != 1 )
      return QStringLiteral( "geostat_input_source_operation_conflicts" );
    decoded.sourceOperationId = sourceOperation.toString();
    if( out )
      *out = decoded;
    return {};
  }

  if( const QString code = decodeSampleIdentity( inputs, &decoded ); !code.isEmpty() )
    return code;
  if( const QString code = decodeGrid( inputs.value( QStringLiteral( "grid" ) ), &decoded.grid );
      !code.isEmpty() )
    return code;
  // 分配前准入：样本数取上限作上界；物化后按实际样本数重算入冻结计划。
  bool estimateOk = false;
  decoded.estimate = estimateGeostatResources( method, decoded.effectiveParams, decoded.grid.cols,
                                               decoded.grid.rows, kGeostatAtomMaxSamples, &estimateOk );
  if( !estimateOk )
    return QStringLiteral( "geostat_resource_overflow" );
  if( const QString code = geostatEstimateAdmission( decoded.estimate ); !code.isEmpty() )
    return code;
  if( out )
    *out = decoded;
  return {};
}

QJsonObject geostatPlanFacts( const GeostatFrozenPlan &plan )
{
  const QVariantMap &unit = plan.unit;
  const QJsonObject unitIdentity{
    { "schemeId", unit.value( QStringLiteral( "schemeId" ) ).toString() },
    { "revision", unit.value( QStringLiteral( "revision" ) ).toString() },
    { "intervalId", unit.value( QStringLiteral( "intervalId" ) ).toString() },
    { "subIntervalId", unit.value( QStringLiteral( "subIntervalId" ) ).toString() } };
  const QJsonObject sample{
    { "versionId", plan.sample.versionId },
    { "sourceSha256", plan.sample.sourceSha256 },
    { "layerKey", plan.sample.layerKey },
    { "association", plan.sample.association },
    { "factorId", plan.sample.factorId },
    { "field", plan.sample.field },
    { "valueUnit", plan.sample.valueUnit },
    { "crsAuthId", plan.sample.crsAuthId },
    { "crsWktSha256", plan.sample.crsWktSha256 },
    { "count", plan.sample.sampleCount },
    { "skippedRows", plan.sample.skippedRows },
    { "snapshotSha256", plan.sample.snapshotSha256 } };
  QJsonObject facts{
    { "schema", plan.schema },
    { "method", plan.method == GeostatMethod::Sgs ? QStringLiteral( "sgs" ) : QStringLiteral( "kriging" ) },
    { "sample", sample },
    { "grid", plan.grid.toJson() },
    { "variogram", plan.variogram.toJson() },
    { "effectiveParameters", QJsonObject::fromVariantMap( plan.effectiveParams) },
    { "unit", unitIdentity },
    { "constraintsApplied", QStringLiteral( "none" ) },
    { "resourceEstimate", plan.estimate.toJson() } };
  if( plan.method == GeostatMethod::Sgs )
    facts.insert( "seed", QString::number( plan.seed ) ); // 精确十进制字符串
  return facts;
}

} // namespace paleo::geostat_atom
