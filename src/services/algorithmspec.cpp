// 层：数据
// services/algorithmspec.cpp — 见 algorithmspec.h；P0 静态登记表。
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

AlgorithmParamField paramString( const QString &key, const QString &label,
                                 const QString &def, bool required )
{
  AlgorithmParamField f;
  f.key = key;
  f.label = label;
  f.type = AlgorithmParamField::String;
  f.defaultValue = def;
  f.required = required;
  return f;
}

AlgorithmParamField paramInt( const QString &key, const QString &label, int def,
                              bool hasMin, double minV, bool hasMax, double maxV )
{
  AlgorithmParamField f;
  f.key = key;
  f.label = label;
  f.type = AlgorithmParamField::Int;
  f.defaultValue = def;
  f.hasMin = hasMin;
  f.minValue = minV;
  f.hasMax = hasMax;
  f.maxValue = maxV;
  return f;
}

AlgorithmParamField paramDouble( const QString &key, const QString &label, double def,
                                 bool hasMin, double minV, bool hasMax, double maxV,
                                 int decimals = 2 )
{
  AlgorithmParamField f;
  f.key = key;
  f.label = label;
  f.type = AlgorithmParamField::Double;
  f.defaultValue = def;
  f.hasMin = hasMin;
  f.minValue = minV;
  f.hasMax = hasMax;
  f.maxValue = maxV;
  f.decimals = decimals;
  return f;
}

// 完整 Spec：对照 ConstraintIDWAlgorithm::initAlgorithm + AlgorithmParamSchema
// + LOCAL_GRID_WKT 隐藏约定（rasterout.h）。
AlgorithmSpec constraintIdwSpec()
{
  AlgorithmSpec s;
  s.id = QStringLiteral( "paleo:paleo_constraint_idw" );
  s.displayName = QStringLiteral( "Paleo: Constraint IDW" );
  s.engine = QStringLiteral( "processing" );
  s.complete = true;

  s.inputs = {
    port( QStringLiteral( "INPUT" ), QStringLiteral( "vector" ), true, false,
          QStringLiteral( "point" ) ),
    port( QStringLiteral( "CONSTRAINTS" ), QStringLiteral( "vector" ), false, false,
          QStringLiteral( "line" ) ),
    // 编排注入；不在 initAlgorithm 登记（见 algorithm-atom-contract.md §2.2）
    port( QStringLiteral( "LOCAL_GRID_WKT" ), QStringLiteral( "string" ), false, true ),
  };

  s.outputs = {
    port( QStringLiteral( "OUTPUT" ), QStringLiteral( "raster" ), true, false ),
    port( QStringLiteral( "CONSTRAINT_RECEIPTS" ), QStringLiteral( "string" ), false, false ),
    port( QStringLiteral( "SAMPLE_RECEIPTS" ), QStringLiteral( "string" ), false, false ),
  };

  // 标量：schema 已有 FIELD/FACIES_CODE/CELL_SIZE；补 ANISO_RATIO（initAlgorithm 有、schema 暂缺）
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

AlgorithmSpec stub( const QString &id, const QString &displayName )
{
  AlgorithmSpec s;
  s.id = id;
  s.displayName = displayName;
  s.engine = QStringLiteral( "processing" );
  s.complete = false; // TODO：后续切片补全端口
  return s;
}

const QVector<AlgorithmSpec> &allSpecs()
{
  static const QVector<AlgorithmSpec> kSpecs = {
    constraintIdwSpec(),
    // 其余 Processing 占位（P0 可选；complete=false，不对齐测试强制）
    stub( QStringLiteral( "paleo:paleo_welldist" ), QStringLiteral( "Paleo: Well Distance" ) ),
    stub( QStringLiteral( "paleo:paleo_distance_transform" ),
          QStringLiteral( "Paleo: Distance Transform" ) ),
    stub( QStringLiteral( "paleo:paleo_min_curvature" ), QStringLiteral( "Paleo: Min Curvature" ) ),
    stub( QStringLiteral( "paleo:paleo_isopach" ), QStringLiteral( "Paleo: Isopach" ) ),
    stub( QStringLiteral( "paleo:paleo_local_direction_idw" ),
          QStringLiteral( "Paleo: Local Direction IDW" ) ),
    stub( QStringLiteral( "paleo:paleo_structural_idw" ),
          QStringLiteral( "Paleo: Structural IDW" ) ),
    stub( QStringLiteral( "paleo:paleo_surfer_idw" ), QStringLiteral( "Paleo: Surfer IDW" ) ),
    stub( QStringLiteral( "paleo:paleo_cartographic_work" ),
          QStringLiteral( "Paleo: Cartographic Work" ) ),
    stub( QStringLiteral( "paleo:paleo_facies_fusion" ), QStringLiteral( "Paleo: Facies Fusion" ) ),
    stub( QStringLiteral( "paleo:paleo_geological_smoothing" ),
          QStringLiteral( "Paleo: Geological Smoothing" ) ),
    stub( QStringLiteral( "paleo:paleo_facies_polygonize" ),
          QStringLiteral( "Paleo: Facies Polygonize" ) ),
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
  // 目的地型输出 = addParameter(…Destination)；string 诊断 = addOutput，不计。
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
