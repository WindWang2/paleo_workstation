// 层：数据
// services/geostaticapabilityspec.cpp — 见 geostaticapabilityspec.h。
#include "geostaticapabilityspec.h"

#include "singlefactordef.h"

#include <QCoreApplication>

namespace paleo::geostat_atom
{
namespace
{

AlgoPort port( const QString &id, const QString &kind, bool required = true,
               const QString &geometryHint = QString() )
{
  AlgoPort p;
  p.id = id;
  p.kind = kind;
  p.required = required;
  p.geometryHint = geometryHint;
  return p;
}

AlgorithmParamField paramDouble( const QString &key, const QString &label, double def,
                                 bool hasMin, double minV )
{
  AlgorithmParamField f;
  f.key = key;
  f.label = label;
  f.type = AlgorithmParamField::Double;
  f.defaultValue = def;
  f.hasMin = hasMin;
  f.minValue = minV;
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

// SEED 走 String：JSON 原生 number 经 QVariant/toDouble 在 >2^53 时丢精度，
// Int 字段又是 32 位截断——十进制字符串是唯一可精确往返的表示。解码层
// （workflow/geostatatomcontract）负责 digits→quint64 的精确校验。
AlgorithmParamField paramSeed()
{
  AlgorithmParamField f;
  f.key = QStringLiteral( "SEED" );
  f.label = QCoreApplication::translate( "GeostatAtomSpec", "随机种子（十进制字符串，精确往返）" );
  f.type = AlgorithmParamField::String;
  f.defaultValue = QStringLiteral( "42" );
  return f;
}

AlgorithmParamField paramVariogramModel()
{
  AlgorithmParamField f;
  f.key = QStringLiteral( "VARIOGRAM_MODEL" );
  f.label = QCoreApplication::translate( "GeostatAtomSpec", "变差模型类型" );
  f.type = AlgorithmParamField::InList;
  f.defaultValue = QStringLiteral( "spherical" );
  for( const auto &name : variogramModelVocabulary() )
  {
    AlgorithmParamOption option;
    option.label = name;
    option.value = name;
    f.options.append( option );
  }
  return f;
}

QVector<AlgorithmParamField> variogramParams()
{
  return { paramVariogramModel(),
           paramDouble( QStringLiteral( "NUGGET" ),
                        QCoreApplication::translate( "GeostatAtomSpec", "块金值（≥0）" ), 0.0, true, 0.0 ),
           paramDouble( QStringLiteral( "SILL" ),
                        QCoreApplication::translate( "GeostatAtomSpec", "部分基台（>0，总基台=块金+部分基台）" ), 1.0, true, 0.0 ),
           paramDouble( QStringLiteral( "RANGE" ),
                        QCoreApplication::translate( "GeostatAtomSpec", "变程（>0，投影平面米制）" ), 1.0, true, 0.0 ),
           paramDouble( QStringLiteral( "ANISOTROPY_RATIO" ),
                        QCoreApplication::translate( "GeostatAtomSpec", "各向异性比（≥1，1=各向同性）" ), 1.0, true, 1.0 ),
           paramDouble( QStringLiteral( "AZIMUTH_DEG" ),
                        QCoreApplication::translate( "GeostatAtomSpec", "长轴方位角（度，[0,360)）" ), 0.0, true, 0.0 ) };
}

// 平面栅格端口描述：具体单位取决于样本因子（m / 无量纲 / mD…），端口只声明
// 单位的语义层级；具体单位由冻结计划的 sample.valueUnit 与成果元数据给出。
AlgoPort rasterPort( const QString &id, const QString &unit, bool required = true )
{
  AlgoPort p = port( id, QStringLiteral( "raster" ), required );
  p.unit = unit;
  p.axis = QStringLiteral( "XY" );
  return p;
}

AlgorithmSpec geostatAtomSpec( const QString &id, const QString &displayName )
{
  AlgorithmSpec s;
  s.id = id;
  s.displayName = displayName;
  s.engine = QStringLiteral( "native_cpp" );
  s.contractVersion = kGeostatAtomAlgorithmContractVersion;
  s.complete = true;
  s.executionShape = AlgorithmExecutionShape::GlobalNode;
  // 固定 seed（SGS）与确定性求解（克里金）都按同一实现指纹可复现。
  s.reproducibility = AlgorithmReproducibility::Deterministic;
  s.cancelSupported = true;
  s.inputs = { port( QStringLiteral( "SAMPLES" ), QStringLiteral( "table" ), true,
                     QStringLiteral( "point" ) ) };
  return s;
}

} // namespace

CapabilitySpec krigingCapabilitySpec()
{
  CapabilitySpec spec;
  spec.capabilityId = kKrigingCapabilityId();
  spec.contractVersion = 1;
  spec.kind = CapabilityKind::Compute;
  spec.algorithmId = kKrigingAlgorithmId();
  spec.algorithmContractVersion = kGeostatAtomAlgorithmContractVersion;
  spec.exposure = CapabilityExposure::Callable;
  spec.exposureReason = QStringLiteral(
      "unconstrained geostatistics over a host-verified managed point sample; "
      "no project constraints, barriers or covariates are consumed" );
  spec.aliases = { kKrigingToolAlias() };
  spec.publishEffect = QStringLiteral( "optional" );
  spec.cancelSupported = true;
  spec.resumeSupported = false; // 无检查点；可从冻结快照确定性重跑
  spec.restartable = true;
  spec.operatorShapes = { QStringLiteral( "global" ) };
  return spec;
}

CapabilitySpec sgsCapabilitySpec()
{
  CapabilitySpec spec = krigingCapabilitySpec();
  spec.capabilityId = kSgsCapabilityId();
  spec.algorithmId = kSgsAlgorithmId();
  spec.aliases = { kSgsToolAlias() };
  return spec;
}

void appendGeostatAtomAlgorithmSpecs( QVector<AlgorithmSpec> *specs )
{
  if( !specs )
    return;
  AlgorithmSpec kriging = geostatAtomSpec( kKrigingAlgorithmId(),
                                           QStringLiteral( "Geostat: Ordinary Kriging (agent v2)" ) );
  kriging.outputs = { rasterPort( QStringLiteral( "OUTPUT" ), QStringLiteral( "factor value unit" ) ),
                      rasterPort( QStringLiteral( "VARIANCE" ),
                                  QStringLiteral( "squared factor value unit" ), false ) };
  kriging.params = variogramParams();
  kriging.params.append( paramInt( QStringLiteral( "MAX_POINTS" ),
                                   QCoreApplication::translate( "GeostatAtomSpec", "邻域点数上限 [1,64]" ),
                                   16, true, 1.0, true, 64.0 ) );
  kriging.params.append( paramInt( QStringLiteral( "MIN_POINTS" ),
                                   QCoreApplication::translate( "GeostatAtomSpec", "半径模式最少邻点 [1,64]（仅 SEARCH_RADIUS>0 生效）" ),
                                   4, true, 1.0, true, 64.0 ) );
  kriging.params.append( paramDouble( QStringLiteral( "SEARCH_RADIUS" ),
                                      QCoreApplication::translate( "GeostatAtomSpec", "搜索半径（米，0=纯最近邻）" ),
                                      0.0, true, 0.0 ) );
  specs->append( kriging );

  AlgorithmSpec sgs = geostatAtomSpec( kSgsAlgorithmId(),
                                       QStringLiteral( "Geostat: Sequential Gaussian Simulation (agent v2)" ) );
  sgs.outputs = { rasterPort( QStringLiteral( "OUTPUT" ), QStringLiteral( "factor value unit" ) ),
                  rasterPort( QStringLiteral( "STD" ),
                              QStringLiteral( "factor value unit (realization spread)" ), false ) };
  sgs.params = variogramParams();
  sgs.params.append( paramInt( QStringLiteral( "REALIZATIONS" ),
                               QCoreApplication::translate( "GeostatAtomSpec", "实现次数 [1,64]" ),
                               4, true, 1.0, true, 64.0 ) );
  sgs.params.append( paramSeed() );
  sgs.params.append( paramInt( QStringLiteral( "MAX_POINTS" ),
                               QCoreApplication::translate( "GeostatAtomSpec", "邻域点数上限 [1,64]" ),
                               16, true, 1.0, true, 64.0 ) );
  sgs.params.append( paramDouble( QStringLiteral( "SEARCH_RADIUS" ),
                                  QCoreApplication::translate( "GeostatAtomSpec", "搜索半径（米，0=全局最近邻）" ),
                                  0.0, true, 0.0 ) );
  specs->append( sgs );
}

bool resolveGeostatFactor( const QString &factorId, QString *unit, QString *field, QString *code )
{
  const auto clear = [unit, field]() {
    if( unit )
      unit->clear();
    if( field )
      field->clear();
  };
  for( const GeostatFactorContract &factor : geostatFactorContracts() )
  {
    if( factor.id != factorId )
      continue;
    // 逐项对照词表当前值（井点口径）：单位、登记字段、井点可提取性。
    bool profileOk = false, definitionOk = false;
    const FactorGeologicalProfile profile =
        SingleFactorRegistry::geologicalProfile( factorId, &profileOk, QStringLiteral( "wells" ) );
    const SingleFactorDefinition definition =
        SingleFactorRegistry::byId( factorId, &definitionOk, QStringLiteral( "wells" ) );
    const QString registeredField =
        definition.defaultParams.value( QStringLiteral( "field" ) ).toString();
    if( !profileOk || !definitionOk || !profile.wellPointExtraction || profile.unit != factor.unit ||
        registeredField != factor.field )
    {
      clear();
      if( code )
        *code = QStringLiteral( "geostat_sample_unit_unconfirmed" );
      return false;
    }
    if( unit )
      *unit = factor.unit;
    if( field )
      *field = factor.field;
    if( code )
      code->clear();
    return true;
  }
  clear();
  if( code )
    *code = QStringLiteral( "geostat_factor_unsupported" );
  return false;
}

} // namespace paleo::geostat_atom
