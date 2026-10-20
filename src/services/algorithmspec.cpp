// 层：数据
// services/algorithmspec.cpp — 见 algorithmspec.h；静态登记表（algo-atom P0/P0-fill）。
#include "algorithmspec.h"

#include <QCoreApplication>

namespace
{

AlgoPort port( const QString &id, const QString &kind, bool required = true,
               bool hidden = false, const QString &geometryHint = QString(),
               const QString &role = QString() )
{
  AlgoPort p;
  p.id = id;
  p.kind = kind;
  p.required = required;
  p.hidden = hidden;
  p.geometryHint = geometryHint;
  p.role = role;
  return p;
}

AlgorithmParamField paramString( const QString &key, const QString &label = QString(),
                                 const QString &def = QString(), bool required = false )
{
  AlgorithmParamField f;
  f.key = key;
  f.label = label.isEmpty() ? key : label;
  f.type = AlgorithmParamField::String;
  f.defaultValue = def;
  f.required = required;
  return f;
}

AlgorithmParamField paramInt( const QString &key, const QString &label, int def,
                              bool hasMin = false, double minV = 0, bool hasMax = false,
                              double maxV = 0 )
{
  AlgorithmParamField f;
  f.key = key;
  f.label = label.isEmpty() ? key : label;
  f.type = AlgorithmParamField::Int;
  f.defaultValue = def;
  f.hasMin = hasMin;
  f.minValue = minV;
  f.hasMax = hasMax;
  f.maxValue = maxV;
  return f;
}

AlgorithmParamField paramDouble( const QString &key, const QString &label, double def,
                                 bool hasMin = false, double minV = 0, bool hasMax = false,
                                 double maxV = 0, int decimals = 2 )
{
  AlgorithmParamField f;
  f.key = key;
  f.label = label.isEmpty() ? key : label;
  f.type = AlgorithmParamField::Double;
  f.defaultValue = def;
  f.hasMin = hasMin;
  f.minValue = minV;
  f.hasMax = hasMax;
  f.maxValue = maxV;
  f.decimals = decimals;
  return f;
}

AlgorithmParamField paramKey( const QString &key, AlgorithmParamField::Type type =
                                                     AlgorithmParamField::Double )
{
  AlgorithmParamField f;
  f.key = key;
  f.label = key;
  f.type = type;
  return f;
}

AlgorithmSpec base( const QString &id, const QString &displayName )
{
  AlgorithmSpec s;
  s.id = id;
  s.displayName = displayName;
  s.engine = QStringLiteral( "processing" );
  s.complete = true;
  return s;
}

AlgorithmSpec constraintIdwSpec()
{
  AlgorithmSpec s = base( QStringLiteral( "paleo:paleo_constraint_idw" ),
                          QStringLiteral( "Paleo: Constraint IDW" ) );
  s.inputs = {
    port( QStringLiteral( "INPUT" ), QStringLiteral( "vector" ), true, false,
          QStringLiteral( "point" ) ),
    port( QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "vector" ), false, false,
          QStringLiteral( "line" ) ),
    port( QStringLiteral( "LOCAL_GRID_WKT" ), QStringLiteral( "string" ), false, true ),
  };
  s.outputs = {
    port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ) ),
    port( QStringLiteral( "CONSTRAINT_RECEIPTS" ), QStringLiteral( "string" ), false ),
    port( QStringLiteral( "SAMPLE_RECEIPTS" ), QStringLiteral( "string" ), false ),
  };
  s.params = {
    paramString( QStringLiteral( "FIELD" ),
                 QCoreApplication::translate( "AlgorithmSpec", "Z 值字段名" ),
                 QStringLiteral( "z" ), true ),
    paramInt( QStringLiteral( "FACIES_CODE" ),
              QCoreApplication::translate( "AlgorithmSpec", "相代码（元数据）" ), 1, true, 0.0,
              true, 99.0 ),
    paramDouble( QStringLiteral( "CELL_SIZE" ),
                 QCoreApplication::translate( "AlgorithmSpec", "网格单元大小" ), 1.0, true,
                 0.0001, false, 0.0, 4 ),
    paramDouble( QStringLiteral( "ANISO_RATIO" ),
                 QCoreApplication::translate( "AlgorithmSpec", "各向异性比（沿方向线）" ), 2.0,
                 true, 0.0, true, 1000.0, 2 ),
  };
  return s;
}

AlgorithmSpec faciesFusionSpec()
{
  AlgorithmSpec s = base( QStringLiteral( "paleo:paleo_facies_fusion" ),
                          QStringLiteral( "Paleo: Facies Fusion" ) );
  s.inputs = {
    port( QStringLiteral( "INPUTS" ), QStringLiteral( "raster" ), true ), // MultipleLayers
  };
  s.outputs = { port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ) ) };
  s.params = {}; // 仅图层型
  return s;
}

AlgorithmSpec geologicalSmoothingSpec()
{
  AlgorithmSpec s = base( QStringLiteral( "paleo:paleo_geological_smoothing" ),
                          QStringLiteral( "Paleo: Geological Smoothing" ) );
  s.inputs = { port( QStringLiteral( "INPUT" ), QStringLiteral( "raster" ) ) };
  s.outputs = { port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ) ) };
  s.params = {
    paramInt( QStringLiteral( "PASSES" ),
              QCoreApplication::translate( "AlgorithmSpec", "滤波次数" ), 1, true, 0.0, true,
              99.0 ),
  };
  return s;
}

AlgorithmSpec faciesPolygonizeSpec()
{
  AlgorithmSpec s = base( QStringLiteral( "paleo:paleo_facies_polygonize" ),
                          QStringLiteral( "Paleo: Facies Polygonize" ) );
  s.inputs = {
    port( QStringLiteral( "INPUT" ), QStringLiteral( "raster" ) ),
    port( QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "vector" ), false, false,
          QStringLiteral( "line" ) ),
  };
  s.outputs = { port( QStringLiteral( "OUTPUT" ), QStringLiteral( "vector" ) ) };
  s.params = {
    paramInt( QStringLiteral( "SMOOTH" ), QString(), 1, true, 0, true, 10 ),
    paramInt( QStringLiteral( "MIN_CELLS" ), QString(), 4, true, 0 ),
    paramDouble( QStringLiteral( "MIN_FRACTION" ), QString(), 0.0025, true, 0, true, 1, 4 ),
    paramDouble( QStringLiteral( "MIN_AREA" ), QString(), 0.0, true, 0 ),
    paramDouble( QStringLiteral( "SIMPLIFY" ), QString(), 0.0, true, 0 ),
    paramInt( QStringLiteral( "BOUNDARY_SMOOTH" ), QString(), 2, true, 0, true, 5 ),
    paramDouble( QStringLiteral( "SNAP_TOLERANCE" ), QString(), 0.0, true, 0 ),
    paramDouble( QStringLiteral( "ANGLE_TOLERANCE" ), QString(), 15.0, true, 0, true, 90 ),
    paramKey( QStringLiteral( "VALUE_RANGES" ), AlgorithmParamField::String ),
  };
  return s;
}

AlgorithmSpec isopachSpec()
{
  AlgorithmSpec s =
      base( QStringLiteral( "paleo:paleo_isopach" ), QStringLiteral( "Paleo: Isopach" ) );
  s.inputs = {
    port( QStringLiteral( "INPUT_TOP" ), QStringLiteral( "raster" ) ),
    port( QStringLiteral( "INPUT_BASE" ), QStringLiteral( "raster" ) ),
    port( QStringLiteral( "LOCAL_GRID_WKT" ), QStringLiteral( "string" ), false, true ),
  };
  s.outputs = { port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ) ) };
  s.params = {
    paramKey( QStringLiteral( "NEGATIVE_TO_NODATA" ), AlgorithmParamField::InList ),
  };
  return s;
}

AlgorithmSpec welldistSpec()
{
  AlgorithmSpec s = base( QStringLiteral( "paleo:paleo_welldist" ),
                          QStringLiteral( "Paleo: Well Distance" ) );
  s.inputs = { port( QStringLiteral( "INPUT" ), QStringLiteral( "vector" ), true, false,
                     QStringLiteral( "point" ) ) };
  s.outputs = { port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ) ) };
  s.params = { paramDouble( QStringLiteral( "CELL_SIZE" ), QString(), 1.0, true, 0.0001 ) };
  return s;
}

AlgorithmSpec distanceTransformSpec()
{
  AlgorithmSpec s = base( QStringLiteral( "paleo:paleo_distance_transform" ),
                          QStringLiteral( "Paleo: Distance Transform" ) );
  s.inputs = {
    port( QStringLiteral( "INPUT" ), QStringLiteral( "vector" ), true, false,
          QStringLiteral( "point" ) ),
    port( QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "vector" ), false, false,
          QStringLiteral( "line" ) ),
    port( QStringLiteral( "LOCAL_GRID_WKT" ), QStringLiteral( "string" ), false, true ),
  };
  s.outputs = {
    port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ) ),
    port( QStringLiteral( "CONSTRAINT_RECEIPTS" ), QStringLiteral( "string" ), false ),
  };
  s.params = { paramDouble( QStringLiteral( "CELL_SIZE" ), QString(), 1.0, true, 0.0001 ) };
  return s;
}

AlgorithmSpec minCurvatureSpec()
{
  AlgorithmSpec s = base( QStringLiteral( "paleo:paleo_min_curvature" ),
                          QStringLiteral( "Paleo: Min Curvature" ) );
  s.inputs = {
    port( QStringLiteral( "INPUT" ), QStringLiteral( "vector" ), true, false,
          QStringLiteral( "point" ) ),
    port( QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "vector" ), false, false,
          QStringLiteral( "line" ) ),
    port( QStringLiteral( "LOCAL_GRID_WKT" ), QStringLiteral( "string" ), false, true ),
  };
  s.outputs = {
    port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ) ),
    port( QStringLiteral( "CONSTRAINT_RECEIPTS" ), QStringLiteral( "string" ), false ),
  };
  s.params = {
    paramString( QStringLiteral( "FIELD" ), QString(), QStringLiteral( "z" ), true ),
    paramDouble( QStringLiteral( "TENSION" ), QString(), 0.0 ),
    paramInt( QStringLiteral( "MAX_SWEEPS" ), QString(), 0 ),
    paramDouble( QStringLiteral( "CELL_SIZE" ), QString(), 1.0, true, 0.0001 ),
  };
  return s;
}

AlgorithmSpec localDirectionIdwSpec()
{
  AlgorithmSpec s = base( QStringLiteral( "paleo:paleo_local_direction_idw" ),
                          QStringLiteral( "Paleo: Local Direction IDW" ) );
  s.inputs = {
    port( QStringLiteral( "INPUT" ), QStringLiteral( "vector" ), true, false,
          QStringLiteral( "point" ) ),
    port( QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "vector" ), false, false,
          QStringLiteral( "line" ) ),
    port( QStringLiteral( "EXTENT" ), QStringLiteral( "scalar" ), false ),
    port( QStringLiteral( "LOCAL_GRID_WKT" ), QStringLiteral( "string" ), false, true ),
  };
  s.outputs = { port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ) ) };
  // 标量 / 枚举：与 initAlgorithm 参数名对齐（类型以 schema 未覆盖者为 key 桩）
  const QStringList scalars = {
    QStringLiteral( "FIELD" ),
    QStringLiteral( "CELL_SIZE" ),
    QStringLiteral( "POWER" ),
    QStringLiteral( "COVERAGE" ),
    QStringLiteral( "CLUSTER" ),
    QStringLiteral( "LOCAL_GRID" ),
    QStringLiteral( "REQUIRE_FULL_COVERAGE" ),
    QStringLiteral( "PERCENT_TO_FRACTION" ),
    QStringLiteral( "MIN_POINTS" ),
    QStringLiteral( "MAX_POINTS" ),
    QStringLiteral( "SEARCH_RADIUS" ),
    QStringLiteral( "METHOD" ),
    QStringLiteral( "VARIAGRAM_MODEL" ),
    QStringLiteral( "NUGGET" ),
    QStringLiteral( "SILL" ),
    QStringLiteral( "RANGE" ),
    QStringLiteral( "VARIAGRAM_AZIMUTH" ),
    QStringLiteral( "KRIGING_MAX_POINTS" ),
    QStringLiteral( "VALUE_UNIT" ),
  };
  for ( const QString &k : scalars )
    s.params.append( paramKey( k ) );
  return s;
}

AlgorithmSpec surferIdwSpec()
{
  AlgorithmSpec s = base( QStringLiteral( "paleo:paleo_surfer_idw" ),
                          QStringLiteral( "Paleo: Surfer IDW" ) );
  s.inputs = {
    port( QStringLiteral( "INPUT" ), QStringLiteral( "vector" ), true, false,
          QStringLiteral( "point" ) ),
    port( QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "vector" ), false, false,
          QStringLiteral( "line" ) ),
    port( QStringLiteral( "EXTENT" ), QStringLiteral( "scalar" ), false ),
    port( QStringLiteral( "LOCAL_GRID_WKT" ), QStringLiteral( "string" ), false, true ),
  };
  s.outputs = { port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ) ) };
  for ( const QString &k : {
         QStringLiteral( "FIELD" ), QStringLiteral( "CELL_SIZE" ), QStringLiteral( "POWER" ),
         QStringLiteral( "COVERAGE" ), QStringLiteral( "LOCAL_GRID" ),
         QStringLiteral( "PERCENT_TO_FRACTION" ), QStringLiteral( "MIN_POINTS" ),
         QStringLiteral( "MAX_POINTS" ), QStringLiteral( "SEARCH_RADIUS" ),
         QStringLiteral( "ANISOTROPY_RATIO" ), QStringLiteral( "ANISOTROPY_ANGLE" ),
         QStringLiteral( "VALUE_UNIT" ) } )
    s.params.append( paramKey( k ) );
  return s;
}

AlgorithmSpec cartographicWorkSpec()
{
  AlgorithmSpec s = base( QStringLiteral( "paleo:paleo_cartographic_work" ),
                          QStringLiteral( "Paleo: Cartographic Work" ) );
  s.inputs = {
    port( QStringLiteral( "INPUT" ), QStringLiteral( "raster" ) ),
    port( QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "vector" ), false, false,
          QStringLiteral( "line" ) ),
  };
  s.outputs = { port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ) ) };
  s.params = {
    paramKey( QStringLiteral( "LEVELS" ) ),
    paramKey( QStringLiteral( "TRANSITION" ) ),
  };
  return s;
}

AlgorithmSpec structuralIdwSpec()
{
  AlgorithmSpec s = base( QStringLiteral( "paleo:paleo_structural_idw" ),
                          QStringLiteral( "Paleo: Structural IDW" ) );
  s.inputs = {
    port( QStringLiteral( "WELLS" ), QStringLiteral( "vector" ), true, false,
          QStringLiteral( "point" ) ),
    port( QStringLiteral( "BOUNDARY" ), QStringLiteral( "vector" ), true, false,
          QStringLiteral( "polygon" ) ),
    port( QStringLiteral( "INTERPOLATION_AREA" ), QStringLiteral( "vector" ), false ),
    port( QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "vector" ), false, false,
          QStringLiteral( "line" ) ),
    port( QStringLiteral( "LOCAL_GRID_WKT" ), QStringLiteral( "string" ), false, true ),
  };
  s.outputs = { port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ) ) };
  for ( const QString &k : {
         QStringLiteral( "WELL_ID_FIELD" ), QStringLiteral( "VALUE_FIELD" ),
         QStringLiteral( "FACTOR_NAME" ), QStringLiteral( "FACTOR_MODE" ),
         QStringLiteral( "NUMERATOR_FIELD" ), QStringLiteral( "DENOMINATOR_FIELD" ),
         QStringLiteral( "VALUE_RANGE" ), QStringLiteral( "GRID_RESOLUTION" ),
         QStringLiteral( "POWER" ), QStringLiteral( "MIN_POINTS" ),
         QStringLiteral( "MAX_POINTS" ), QStringLiteral( "SEARCH_RADIUS" ),
         QStringLiteral( "EXTEND_TO_BOUNDARY" ), QStringLiteral( "ENABLE_BARRIERS" ),
         QStringLiteral( "ENABLE_DIRECTIONS" ), QStringLiteral( "INTERPRETIVE_STRENGTH" ),
         QStringLiteral( "BARRIER_BUFFER" ), QStringLiteral( "BARRIER_BUFFER_AUTO" ),
         QStringLiteral( "BARRIER_SHAPE_RADIUS" ), QStringLiteral( "BARRIER_SHAPE_STRENGTH" ),
         QStringLiteral( "BARRIER_EXTEND" ), QStringLiteral( "BARRIER_EXTENSION_LIMIT" ),
         QStringLiteral( "CONTOUR_STOP_BUFFER" ), QStringLiteral( "CLUSTER" ),
         QStringLiteral( "LOCAL_GRID" ) } )
    s.params.append( paramKey( k ) );
  return s;
}

const QVector<AlgorithmSpec> &allSpecs()
{
  static const QVector<AlgorithmSpec> kSpecs = {
    constraintIdwSpec(),
    welldistSpec(),
    distanceTransformSpec(),
    minCurvatureSpec(),
    isopachSpec(),
    localDirectionIdwSpec(),
    structuralIdwSpec(),
    surferIdwSpec(),
    cartographicWorkSpec(),
    faciesFusionSpec(),
    geologicalSmoothingSpec(),
    faciesPolygonizeSpec(),
  };
  return kSpecs;
}

} // namespace

QStringList AlgorithmSpec::processingParameterNames() const
{
  QStringList names;
  auto push = [&names]( const QString &id ) {
    if ( !names.contains( id ) )
      names.append( id );
  };
  for ( const AlgoPort &p : inputs )
  {
    if ( !p.hidden )
      push( p.id );
  }
  for ( const AlgorithmParamField &f : params )
    push( f.key );
  const QStringList destKinds = { QStringLiteral( "raster" ), QStringLiteral( "vector" ),
                                  QStringLiteral( "file" ), QStringLiteral( "table" ) };
  for ( const AlgoPort &p : outputs )
  {
    if ( !p.hidden && destKinds.contains( p.kind ) )
      push( p.id );
  }
  return names;
}

QStringList AlgorithmSpec::hiddenParameterNames() const
{
  QStringList names;
  for ( const AlgoPort &p : inputs )
  {
    if ( p.hidden )
      names.append( p.id );
  }
  for ( const AlgoPort &p : outputs )
  {
    if ( p.hidden )
      names.append( p.id );
  }
  return names;
}

QStringList AlgorithmSpec::paramKeys() const
{
  QStringList keys;
  keys.reserve( params.size() );
  for ( const AlgorithmParamField &f : params )
    keys.append( f.key );
  return keys;
}

bool AlgorithmSpecRegistry::knows( const QString &algorithmId )
{
  for ( const AlgorithmSpec &s : allSpecs() )
  {
    if ( s.id == algorithmId )
      return true;
  }
  return false;
}

AlgorithmSpec AlgorithmSpecRegistry::spec( const QString &algorithmId )
{
  for ( const AlgorithmSpec &s : allSpecs() )
  {
    if ( s.id == algorithmId )
      return s;
  }
  return {};
}

QStringList AlgorithmSpecRegistry::knownIds()
{
  QStringList ids;
  for ( const AlgorithmSpec &s : allSpecs() )
    ids.append( s.id );
  return ids;
}

QStringList AlgorithmSpecRegistry::completeIds()
{
  QStringList ids;
  for ( const AlgorithmSpec &s : allSpecs() )
  {
    if ( s.complete )
      ids.append( s.id );
  }
  return ids;
}
