// 层：测试壳
// tests/tst_geostat_atom_contract.cpp — I1 地统计原子契约测试（W1/W2/W3/W4）：
// 注册表一致性（v2 精确契约、v1 不变、别名无冲突）、严格参数/种子/网格
// 解码、样本因子与词表一致、资源估算与预算拒绝、冻结计划 JSON 往返与
// 防篡改、工具 schema 与解码同义。
#include <QtTest>
#include "workflow/geostatatomcontract.h"
#include "services/geostaticapabilityspec.h"
#include "services/capabilityspec.h"
#include "services/algorithmspec.h"
#include "ai/chat/domaintools.h"
#include <QJsonDocument>
#include <cmath>
#include <limits>

using namespace paleo::geostat_atom;

namespace {

QVariantMap gridMap( double minX, double maxX, double minY, double maxY, double cell )
{
  return QVariantMap{ { "min_x", minX }, { "max_x", maxX },
                      { "min_y", minY }, { "max_y", maxY }, { "cell_size", cell } };
}
QVariantMap unitMap( const QString &sub = QStringLiteral( "S1" ) )
{
  return QVariantMap{ { "schemeId", QStringLiteral( "scheme" ) },
                      { "revision", QStringLiteral( "1" ) },
                      { "intervalId", QStringLiteral( "T70-T71" ) },
                      { "subIntervalId", sub } };
}
QVariantMap baseInputs()
{
  return QVariantMap{ { "sample_ref", QStringLiteral( "catalog:v-1" ) },
                      { "factor_id", QStringLiteral( "sandthick" ) },
                      { "unit_selection", unitMap() },
                      { "grid", gridMap( 0, 100, 0, 100, 10 ) } };
}
QVariantMap krigingParams()
{
  return QVariantMap{ { "VARIOGRAM_MODEL", QStringLiteral( "spherical" ) },
                      { "NUGGET", 0.0 }, { "SILL", 1.0 }, { "RANGE", 40.0 } };
}
QVariantMap sgsParams( int realizations = 3, const QString &seed = QStringLiteral( "42" ) )
{
  QVariantMap params = krigingParams();
  params.insert( "REALIZATIONS", realizations );
  params.insert( "SEED", seed );
  return params;
}
QString decode( GeostatMethod method, const QVariantMap &params, const QVariantMap &inputs,
                const QString &intent, GeostatDecodedRequest *out )
{
  return decodeGeostatAtomRequest( method, params, inputs, intent, out );
}
QString digest( QChar fill )
{
  return QString( 64, fill );
}
// 以解码结果装配一份字段齐全的冻结计划（物化后形态；samples=实际样本数）。
GeostatFrozenPlan frozenPlanFrom( const GeostatDecodedRequest &decoded, int samples )
{
  GeostatFrozenPlan plan;
  plan.method = decoded.method;
  plan.effectiveParams = decoded.effectiveParams;
  plan.variogram = decoded.variogram;
  plan.grid = decoded.grid;
  plan.seed = decoded.seed;
  plan.unit = unitMap();
  plan.sample.versionId = QStringLiteral( "v-1" );
  plan.sample.sourceSha256 = digest( 'a' );
  plan.sample.layerKey = QStringLiteral( "wells.T70" );
  plan.sample.association = QStringLiteral( "layer_key" );
  plan.sample.factorId = decoded.factorId;
  plan.sample.field = decoded.factorField;
  plan.sample.valueUnit = decoded.factorUnit;
  plan.sample.crsAuthId = QStringLiteral( "EPSG:3857" );
  plan.sample.crsWktSha256 = digest( 'b' );
  plan.sample.snapshotSha256 = digest( 'c' );
  plan.sample.snapshotPath = geostatSnapshotRelativePath( plan.sample.snapshotSha256 );
  plan.sample.sampleCount = samples;
  plan.sample.skippedRows = 2;
  bool ok = false;
  plan.estimate = estimateGeostatResources( plan.method, plan.effectiveParams, plan.grid.cols,
                                            plan.grid.rows, samples, &ok );
  return plan;
}

} // namespace

class GeostatAtomContractTests : public QObject
{
  Q_OBJECT
private slots:
  void registryRegistersExactVersionAndKeepsV1Untouched()
  {
    // v2 精确契约：agent 面引用的算法版本存在且 complete。
    const AlgorithmSpec kriging = AlgorithmSpecRegistry::specAtVersion( kKrigingAlgorithmId(), 2 );
    QVERIFY( kriging.complete );
    QCOMPARE( kriging.contractVersion, 2 );
    const AlgorithmSpec sgs = AlgorithmSpecRegistry::specAtVersion( kSgsAlgorithmId(), 2 );
    QVERIFY( sgs.complete );
    // 未知版本不回落：specAtVersion(id,3) 为空；spec(id)（首项 v1）仍可用。
    QVERIFY( AlgorithmSpecRegistry::specAtVersion( kKrigingAlgorithmId(), 3 ).id.isEmpty() );
    const AlgorithmSpec v1 = AlgorithmSpecRegistry::spec( kSgsAlgorithmId() );
    QCOMPARE( v1.contractVersion, 1 );
    // v1 语义保持：SGS v1 仍有 MEMBERS 表端口、无 NUGGET 参数（UI 口径不动）。
    bool membersPort = false;
    for( const auto &port : v1.outputs )
      if( port.id == QLatin1String( "MEMBERS" ) )
        membersPort = true;
    QVERIFY( membersPort );
    bool v1HasNugget = false;
    for( const auto &field : v1.params )
      if( field.key == QLatin1String( "NUGGET" ) )
        v1HasNugget = true;
    QVERIFY( !v1HasNugget );
    // v2 反之：SGS v2 无 MEMBERS 端口（首版只开放 OUTPUT/STD），有显式变差参数。
    bool v2MembersPort = false, v2Nugget = false;
    for( const auto &port : sgs.outputs )
      if( port.id == QLatin1String( "MEMBERS" ) )
        v2MembersPort = true;
    for( const auto &field : sgs.params )
      if( field.key == QLatin1String( "NUGGET" ) )
        v2Nugget = true;
    QVERIFY( !v2MembersPort );
    QVERIFY( v2Nugget );
    // 首版端口集固定：克里金 OUTPUT/VARIANCE，SGS OUTPUT/STD。
    QStringList krigingPorts, sgsPorts;
    for( const auto &port : kriging.outputs )
      krigingPorts.append( port.id );
    for( const auto &port : sgs.outputs )
      sgsPorts.append( port.id );
    krigingPorts.sort();
    sgsPorts.sort();
    QCOMPARE( krigingPorts, ( QStringList{ QStringLiteral( "OUTPUT" ), QStringLiteral( "VARIANCE" ) } ) );
    // 端口描述如实：单位语义层级与平面轴（具体单位由冻结值给出）。
    for( const auto &port : kriging.outputs )
    {
      QCOMPARE( port.axis, QStringLiteral( "XY" ) );
      QVERIFY2( port.unit.contains( QStringLiteral( "factor value unit" ) ), qPrintable( port.id ) );
    }
    const QString varianceUnit =
        kriging.outputs.at( 1 ).id == QLatin1String( "VARIANCE" ) ? kriging.outputs.at( 1 ).unit
                                                                  : kriging.outputs.at( 0 ).unit;
    QVERIFY( varianceUnit.startsWith( QStringLiteral( "squared" ) ) ); // 克里金方差是平方单位
    QCOMPARE( sgsPorts, ( QStringList{ QStringLiteral( "OUTPUT" ), QStringLiteral( "STD" ) } ) );
  }

  void capabilityRowsResolveWithoutAliasCollisions()
  {
    QVERIFY( CapabilityRegistry::aliasCollisions().isEmpty() );
    const CapabilitySpec kriging = CapabilityRegistry::spec( kKrigingCapabilityId(), 1 );
    QCOMPARE( kriging.algorithmId, kKrigingAlgorithmId() );
    QCOMPARE( kriging.algorithmContractVersion, 2 );
    QCOMPARE( kriging.exposure, CapabilityExposure::Callable );
    QVERIFY( kriging.cancelSupported );
    QVERIFY( !kriging.resumeSupported ); // 无检查点：从冻结计划确定性重跑，不算恢复
    QString code;
    QCOMPARE( CapabilityRegistry::resolveAlias( kKrigingToolAlias(), &code ), kKrigingCapabilityId() );
    QVERIFY( code.isEmpty() );
    QCOMPARE( CapabilityRegistry::resolveAlias( kSgsToolAlias(), &code ), kSgsCapabilityId() );
    QVERIFY( CapabilityRegistry::callableIds().contains( kSgsCapabilityId() ) );
    QVERIFY( CapabilityRegistry::executionEffects( kKrigingCapabilityId(), 1 ) == 0 );
    // 精确版本：不存在的能力 v2 行为空（不静默用 v1）。
    QVERIFY( CapabilityRegistry::spec( kKrigingCapabilityId(), 2 ).capabilityId.isEmpty() );
    // 旧 v1 算法 id 不因本轮获得可调用能力（exposure 维持 typed native / internal）。
    QString reason;
    const CapabilityExposure v1Exposure = CapabilityRegistry::exposureForAlgorithm( kSgsAlgorithmId(), &reason );
    QVERIFY( v1Exposure != CapabilityExposure::Callable );
  }

  void defaultsComeFromAlgorithmSpecSingleSource()
  {
    GeostatDecodedRequest decoded;
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), baseInputs(), "compute-only", &decoded ),
              QString() );
    QCOMPARE( decoded.effectiveParams.value( "MAX_POINTS" ).toInt(), 16 );
    QCOMPARE( decoded.effectiveParams.value( "MIN_POINTS" ).toInt(), 4 );
    QCOMPARE( decoded.effectiveParams.value( "SEARCH_RADIUS" ).toDouble(), 0.0 );
    QCOMPARE( decoded.effectiveParams.value( "ANISOTROPY_RATIO" ).toDouble(), 1.0 );
    QCOMPARE( decoded.variogram.model, QStringLiteral( "spherical" ) );
    QCOMPARE( decoded.grid.cols, 10 );
    QCOMPARE( decoded.grid.rows, 10 );
    QCOMPARE( decoded.grid.pixelHeight, -10.0 ); // north-up
    QCOMPARE( decoded.grid.originY, 100.0 );
    QCOMPARE( decoded.factorUnit, QStringLiteral( "m" ) );
    QCOMPARE( decoded.factorField, QStringLiteral( "sand_thick" ) );
    // 空 publish_intent（宿主直调路径）按 compute-only；经聊天入口时意图恒非空。
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), baseInputs(), QString(), &decoded ),
              QString() );
    // 跨度非整数倍：ceil 覆盖整个 extent（东/南最多外扩一个像元）。
    QVariantMap inputs = baseInputs();
    inputs.insert( "grid", gridMap( 0, 95, 0, 100, 10 ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QString() );
    QCOMPARE( decoded.grid.cols, 10 );
    QCOMPARE( decoded.grid.rows, 10 );
  }

  void invalidParametersAreRejectedBeforeAnyMaterialization()
  {
    struct Case
    {
      QString name;
      QVariantMap params;
      QString expected;
    };
    const QVariantMap base = krigingParams();
    const auto with = [&base]( const QString &key, const QVariant &value ) {
      QVariantMap copy = base;
      copy.insert( key, value );
      return copy;
    };
    const QList<Case> cases{
      { QStringLiteral( "unknown_param" ), with( "POWER", 2.0 ),
        QStringLiteral( "geostat_param_unknown_param" ) },
      { QStringLiteral( "bad_model" ), with( "VARIOGRAM_MODEL", QStringLiteral( "cubic" ) ),
        QStringLiteral( "geostat_param_out_of_range" ) },
      { QStringLiteral( "text_number" ), with( "NUGGET", QStringLiteral( "0.5" ) ),
        QStringLiteral( "geostat_param_type_mismatch" ) },
      { QStringLiteral( "fractional_int" ), with( "MAX_POINTS", 16.5 ),
        QStringLiteral( "geostat_param_not_integer" ) },
      { QStringLiteral( "max_points_over" ), with( "MAX_POINTS", 65 ),
        QStringLiteral( "geostat_param_out_of_range" ) },
      { QStringLiteral( "zero_sill" ), with( "SILL", 0.0 ),
        QStringLiteral( "geostat_variogram_out_of_range" ) },
      { QStringLiteral( "zero_range" ), with( "RANGE", 0.0 ),
        QStringLiteral( "geostat_variogram_out_of_range" ) },
      { QStringLiteral( "anisotropy_below_one" ), with( "ANISOTROPY_RATIO", 0.5 ),
        QStringLiteral( "geostat_param_out_of_range" ) },
      { QStringLiteral( "azimuth_360" ), with( "AZIMUTH_DEG", 360.0 ),
        QStringLiteral( "geostat_variogram_out_of_range" ) },
      { QStringLiteral( "boolean_value" ), with( "SILL", true ),
        QStringLiteral( "geostat_param_type_mismatch" ) },
      { QStringLiteral( "members_param" ), with( "MEMBERS", true ),
        QStringLiteral( "geostat_param_unsupported" ) },
      { QStringLiteral( "persist_realizations_param" ), with( "PERSIST_REALIZATIONS", true ),
        QStringLiteral( "geostat_param_unsupported" ) },
    };
    for( const auto &entry : cases )
    {
      GeostatDecodedRequest decoded;
      const QString code =
          decode( GeostatMethod::Kriging, entry.params, baseInputs(), "compute-only", &decoded );
      QVERIFY2( code == entry.expected, qPrintable( entry.name + QStringLiteral( " -> " ) + code ) );
      QVERIFY( decoded.effectiveParams.isEmpty() ); // 失败请求不残留半截有效参数
    }
    // SGS：实现数越界与非法种子。
    GeostatDecodedRequest decoded;
    QCOMPARE( decode( GeostatMethod::Sgs, sgsParams( 65 ), baseInputs(), "compute-only", &decoded ),
              QStringLiteral( "geostat_param_out_of_range" ) );
    QCOMPARE( decode( GeostatMethod::Sgs, sgsParams( 0 ), baseInputs(), "compute-only", &decoded ),
              QStringLiteral( "geostat_param_out_of_range" ) );
    QCOMPARE( decode( GeostatMethod::Sgs, sgsParams( 3, QStringLiteral( "-1" ) ), baseInputs(),
                      "compute-only", &decoded ),
              QStringLiteral( "geostat_param_seed_invalid" ) );
    QCOMPARE( decode( GeostatMethod::Sgs, sgsParams( 3, QStringLiteral( "1e3" ) ), baseInputs(),
                      "compute-only", &decoded ),
              QStringLiteral( "geostat_param_seed_invalid" ) );
    // 巨整 JSON 原生 number（>2^53）拒绝：不经浮点。
    QVariantMap giant = sgsParams();
    giant.insert( "SEED", 1e19 );
    QCOMPARE( decode( GeostatMethod::Sgs, giant, baseInputs(), "compute-only", &decoded ),
              QStringLiteral( "geostat_param_seed_invalid" ) );
  }

  void seedRoundTripsExactlyBeyondDoublePrecision()
  {
    QVERIFY( parseGeostatSeed( QStringLiteral( "18446744073709551615" ), nullptr ) );
    quint64 seed = 0;
    QVERIFY( parseGeostatSeed( QStringLiteral( "9007199254740993" ), &seed ) );
    QCOMPARE( seed, Q_UINT64_C( 9007199254740993 ) ); // 2^53+1：double 表示会丢 1
    QVERIFY( !parseGeostatSeed( QStringLiteral( "18446744073709551616" ), nullptr ) ); // >UINT64_MAX
    QVERIFY( !parseGeostatSeed( QStringLiteral( "-1" ), nullptr ) );
    QVERIFY( !parseGeostatSeed( QStringLiteral( "0x10" ), nullptr ) );
    QVERIFY( !parseGeostatSeed( QStringLiteral( " 1" ), nullptr ) );
    QVERIFY( !parseGeostatSeed( QVariant(), nullptr ) );
    // ≤2^53 的 JSON 原生整值无损。
    QVERIFY( parseGeostatSeed( 9007199254740992.0, &seed ) );
    QCOMPARE( seed, Q_UINT64_C( 9007199254740992 ) );
    QVERIFY( !parseGeostatSeed( 9007199254740994.0, nullptr ) );
    QVERIFY( !parseGeostatSeed( 4.5, nullptr ) );

    GeostatDecodedRequest decoded;
    const QString big = QStringLiteral( "18446744073709551615" );
    QCOMPARE( decode( GeostatMethod::Sgs, sgsParams( 2, big ), baseInputs(), "compute-only", &decoded ),
              QString() );
    QCOMPARE( decoded.seed, Q_UINT64_C( 18446744073709551615 ) );
    // 冻结计划往返与模型可见事实都保持精确十进制字符串。
    const GeostatFrozenPlan plan = frozenPlanFrom( decoded, 16 );
    QCOMPARE( plan.toJson().value( "seed" ).toString(), big );
    bool ok = false;
    const GeostatFrozenPlan restored = GeostatFrozenPlan::fromJson( plan.toJson(), &ok );
    QVERIFY( ok );
    QCOMPARE( restored.seed, Q_UINT64_C( 18446744073709551615 ) );
    QCOMPARE( restored, plan );
    QCOMPARE( geostatPlanFacts( restored ).value( "seed" ).toString(), big );
    QVERIFY( validateGeostatFrozenPlan( restored ).isEmpty() );
  }

  void gridBudgetsRejectBeforeAnyAllocation()
  {
    GeostatDecodedRequest decoded;
    // 200×200 米、cell 0.05 → 4000×4000 = 1600 万格 > 400 万上限。
    QVariantMap inputs = baseInputs();
    inputs.insert( "grid", gridMap( 0, 200, 0, 200, 0.05 ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_grid_cell_budget_exceeded" ) );
    inputs.insert( "grid", gridMap( 0, 0, 0, 100, 10 ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_grid_extent_degenerate" ) );
    inputs.insert( "grid", gridMap( 10, 0, 0, 100, 10 ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_grid_extent_degenerate" ) );
    inputs.insert( "grid", gridMap( 0, std::nan( "1" ), 0, 100, 10 ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_grid_not_finite" ) );
    inputs.insert( "grid", gridMap( 0, 2000000, 0, 10, 10 ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_grid_side_exceeded" ) );
    inputs.insert( "grid", gridMap( 0, 100, 0, 100, 0 ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_grid_cell_size_invalid" ) );
    // 极小像元：维数先撞单边上限；除到 inf 才是维数溢出（有限域检查）。
    inputs.insert( "grid", gridMap( 0, 100, 0, 100, 1e-300 ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_grid_side_exceeded" ) );
    inputs.insert( "grid", gridMap( 0, 100, 0, 100, std::numeric_limits<double>::denorm_min() ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_grid_dimension_overflow" ) );
    // 网格缺键/多键。
    QVariantMap partial = gridMap( 0, 100, 0, 100, 10 );
    partial.remove( "cell_size" );
    inputs.insert( "grid", partial );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_grid_unknown_field" ) );
    // SGS 实现内存预算：64 实现 × 2000×2000 × 8B = 2 GiB 超 512 MiB 上界。
    QVariantMap sgsInputs = baseInputs();
    sgsInputs.insert( "grid", gridMap( 0, 200, 0, 200, 0.1 ) );
    QCOMPARE( decode( GeostatMethod::Sgs, sgsParams( 64 ), sgsInputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_realization_budget_exceeded" ) );
    // 域内（16 实现 × 400 万 × 8B ≈ 488 MiB）通过。
    QCOMPARE( decode( GeostatMethod::Sgs, sgsParams( 16 ), sgsInputs, "compute-only", &decoded ),
              QString() );
    QCOMPARE( decoded.estimate.realizationBytes, qint64( 16 ) * 4000000 * 8 );
  }

  void resourceEstimateIsCheckedArithmeticAndAdmitted()
  {
    bool ok = false;
    // 克里金 10×10、100 样本的逐项口径。
    const GeostatResourceEstimate kriging =
        estimateGeostatResources( GeostatMethod::Kriging, QVariantMap{}, 10, 10, 100, &ok );
    QVERIFY( ok );
    QCOMPARE( kriging.sampleBytes, qint64( 100 * ( 24 + 48 ) ) );
    QCOMPARE( kriging.gridBytes, qint64( 100 * 16 ) );
    QCOMPARE( kriging.realizationBytes, qint64( 0 ) );
    QCOMPARE( kriging.scratchBytes, qint64( 100 * 8 + 64 * 64 * 8 * 4 ) );
    QCOMPARE( kriging.peakBytes, kriging.sampleBytes + kriging.gridBytes + kriging.scratchBytes );
    QCOMPARE( kriging.snapshotBytes, qint64( 100 * 80 + 4096 ) );
    QCOMPARE( kriging.outputBytes, qint64( 100 * 8 + 2 * 65536 ) );
    QVERIFY( geostatEstimateAdmission( kriging ).isEmpty() );
    // SGS 3 实现：实现数组 3×100×8B 计入峰值。
    const GeostatResourceEstimate sgs = estimateGeostatResources(
        GeostatMethod::Sgs, QVariantMap{ { "REALIZATIONS", 3 } }, 10, 10, 100, &ok );
    QVERIFY( ok );
    QCOMPARE( sgs.realizationBytes, qint64( 3 * 100 * 8 ) );
    QCOMPARE( sgs.peakBytes, kriging.peakBytes + qint64( 3 * 100 * 8 ) );
    // JSON 往返逐字段一致。
    GeostatResourceEstimate restored;
    QVERIFY( GeostatResourceEstimate::fromJson( sgs.toJson(), &restored ) );
    QCOMPARE( restored, sgs );
    // 非法输入（负数/越界样本数/非法实现数）不产出估算。
    QVERIFY( !estimateGeostatResources( GeostatMethod::Kriging, QVariantMap{}, 0, 10, 100, &ok ).sampleBytes && !ok );
    QVERIFY( !estimateGeostatResources( GeostatMethod::Kriging, QVariantMap{}, 10, 10,
                                        kGeostatAtomMaxSamples + 1, &ok ).sampleBytes && !ok );
    QVERIFY( !estimateGeostatResources( GeostatMethod::Sgs, QVariantMap{ { "REALIZATIONS", 0 } }, 10, 10,
                                        100, &ok ).sampleBytes && !ok );
    // 各上限的准入码。
    GeostatResourceEstimate over;
    over.realizationBytes = kGeostatAtomMaxRealizationBytes + 1;
    QCOMPARE( geostatEstimateAdmission( over ), QStringLiteral( "geostat_realization_budget_exceeded" ) );
    over = GeostatResourceEstimate{};
    over.peakBytes = kGeostatAtomMaxPeakBytes + 1;
    QCOMPARE( geostatEstimateAdmission( over ), QStringLiteral( "geostat_peak_budget_exceeded" ) );
    over = GeostatResourceEstimate{};
    over.snapshotBytes = kGeostatAtomMaxSnapshotBytes + 1;
    QCOMPARE( geostatEstimateAdmission( over ), QStringLiteral( "geostat_snapshot_budget_exceeded" ) );
    over = GeostatResourceEstimate{};
    over.outputBytes = kGeostatAtomMaxOutputBytes + 1;
    QCOMPARE( geostatEstimateAdmission( over ), QStringLiteral( "geostat_output_budget_exceeded" ) );
    // 解码阶段的估算以样本上限作上界。
    GeostatDecodedRequest decoded;
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), baseInputs(), "compute-only", &decoded ),
              QString() );
    QCOMPARE( decoded.estimate.sampleBytes,
              qint64( kGeostatAtomMaxSamples * ( 24 + 48 ) ) );
    QVERIFY( decoded.estimate.peakBytes <= kGeostatAtomMaxPeakBytes );
  }

  void factorContractComesFromTheRegistry()
  {
    QString unit, field, code;
    // 准入清单逐项与单因素词表（井点口径）一致：单位、登记字段、井点可提取。
    for( const GeostatFactorContract &factor : geostatFactorContracts() )
    {
      unit.clear();
      field.clear();
      code = QStringLiteral( "unset" );
      QVERIFY2( resolveGeostatFactor( factor.id, &unit, &field, &code ), qPrintable( factor.id ) );
      QVERIFY( code.isEmpty() );
      QCOMPARE( unit, factor.unit );
      QCOMPARE( field, factor.field );
    }
    // 词表单位未确认（poro：「小数或 %」）与不可由井点提取的因子首版不开放。
    for( const auto &id : { QStringLiteral( "poro" ), QStringLiteral( "welldist" ),
                            QStringLiteral( "confidence" ), QStringLiteral( "unknown" ) } )
    {
      unit = QStringLiteral( "stale" );
      code.clear();
      QVERIFY( !resolveGeostatFactor( id, &unit, &field, &code ) );
      QCOMPARE( code, QStringLiteral( "geostat_factor_unsupported" ) );
      QVERIFY( unit.isEmpty() ); // 失败不残留输出
    }
    // 解码层同码：未知因子、显式字段与登记字段不符。
    GeostatDecodedRequest decoded;
    QVariantMap inputs = baseInputs();
    inputs.insert( "factor_id", QStringLiteral( "poro" ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_factor_unsupported" ) );
    inputs = baseInputs();
    inputs.insert( "field", QStringLiteral( "poro" ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_field_mismatch" ) );
    // 显式字段与登记字段相同 → 通过。
    inputs.insert( "field", QStringLiteral( "sand_thick" ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QString() );
    QCOMPARE( decoded.factorField, QStringLiteral( "sand_thick" ) );
  }

  void firstVersionBoundariesAreExplicit()
  {
    GeostatDecodedRequest decoded;
    QVariantMap inputs = baseInputs();
    // 首版明确 unsupported：constraints_ref / 协变量 / 屏障 / 任意样本数组。
    inputs.insert( "constraints_ref", QStringLiteral( "catalog:v-2" ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_unsupported" ) );
    inputs.remove( "constraints_ref" );
    inputs.insert( "covariate_ref", QStringLiteral( "x" ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_unsupported" ) );
    inputs.remove( "covariate_ref" );
    inputs.insert( "barriers", QVariantList{} );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_unsupported" ) );
    inputs.remove( "barriers" );
    inputs.insert( "samples", QVariantList{ QVariantMap{ { "x", 0.0 }, { "y", 0.0 }, { "z", 1.0 } } } );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_unsupported" ) );
    inputs.remove( "samples" );
    inputs.insert( "members", QVariantList{} );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_unsupported" ) );
    inputs.remove( "members" );
    inputs.insert( "persist_realizations", true );
    QCOMPARE( decode( GeostatMethod::Sgs, sgsParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_unsupported" ) );
    // 未知输入键（含权限自称键）。
    inputs.remove( "persist_realizations" );
    inputs.insert( "approved", true );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_unknown" ) );
    inputs = baseInputs();
    inputs.insert( "grid_wkt", QStringLiteral( "EPSG:4326" ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inputs, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_unknown" ) );
    // publish 无 source_operation_id：拒绝（publish 只发布已计算结果）。
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), baseInputs(), "publish", &decoded ),
              QStringLiteral( "publish_requires_source_operation" ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), baseInputs(), "stage", &decoded ),
              QStringLiteral( "unsupported_intent" ) );
    // compute-only 继承只接受单键 source_operation_id。
    QVariantMap inherit;
    inherit.insert( "source_operation_id", QStringLiteral( "op-1" ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inherit, "compute-only", &decoded ),
              QString() );
    QCOMPARE( decoded.sourceOperationId, QStringLiteral( "op-1" ) );
    inherit.insert( "sample_ref", QStringLiteral( "catalog:v-1" ) );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inherit, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_source_operation_conflicts" ) );
    inherit.remove( "sample_ref" );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), inherit, "prepare", &decoded ),
              QStringLiteral( "geostat_input_source_operation_misplaced" ) );
    // 缺因子/缺单元/缺网格各自可行动。
    QVariantMap missing = baseInputs();
    missing.remove( "factor_id" );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), missing, "compute-only", &decoded ),
              QStringLiteral( "geostat_factor_unsupported" ) );
    missing = baseInputs();
    missing.remove( "unit_selection" );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), missing, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_unit_invalid" ) );
    missing = baseInputs();
    missing.remove( "grid" );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), missing, "compute-only", &decoded ),
              QStringLiteral( "geostat_grid_missing" ) );
    // 单元身份不完整（Boundary/无子层段）与 aggregate 拒绝。
    QVariantMap boundary = baseInputs();
    QVariantMap unit = unitMap();
    unit.remove( "subIntervalId" );
    boundary.insert( "unit_selection", unit );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), boundary, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_unit_incomplete" ) );
    unit = unitMap();
    unit.insert( "aggregate", true );
    boundary.insert( "unit_selection", unit );
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), boundary, "compute-only", &decoded ),
              QStringLiteral( "geostat_input_unit_incomplete" ) );
  }

  void frozenPlanRejectsTampering()
  {
    GeostatDecodedRequest decoded;
    QCOMPARE( decode( GeostatMethod::Sgs, sgsParams(), baseInputs(), "prepare", &decoded ), QString() );
    const GeostatFrozenPlan plan = frozenPlanFrom( decoded, 25 );
    QVERIFY2( validateGeostatFrozenPlan( plan ).isEmpty(),
              qPrintable( validateGeostatFrozenPlan( plan ) ) );
    bool ok = false;
    QVERIFY( GeostatFrozenPlan::fromJson( plan.toJson(), &ok ).method == GeostatMethod::Sgs && ok );
    const auto broken = [&]( const QString &name, const QJsonObject &json ) {
      bool parsed = true;
      const GeostatFrozenPlan restored = GeostatFrozenPlan::fromJson( json, &parsed );
      const QString code = parsed ? validateGeostatFrozenPlan( restored ) : QStringLiteral( "parse" );
      QVERIFY2( !code.isEmpty(), qPrintable( name + QStringLiteral( " 被接受: " ) + code ) );
    };
    QJsonObject json = plan.toJson();
    // retention 引用与 sample.* 不一致。
    QJsonObject tampered = json;
    tampered.insert( "sourceVerification", QJsonObject{ { "path", QStringLiteral( "../../etc/passwd" ) } } );
    broken( QStringLiteral( "sourceVerification" ), tampered );
    // 快照路径越界（摘要与路径不对应）。
    tampered = json;
    QJsonObject sample = json.value( "sample" ).toObject();
    sample.insert( "snapshot_path", QStringLiteral( "../../outside.json" ) );
    tampered.insert( "sample", sample );
    broken( QStringLiteral( "snapshot_path" ), tampered );
    tampered = json;
    sample = json.value( "sample" ).toObject();
    sample.insert( "snapshot_path", geostatSnapshotRelativePath( digest( 'd' ) ) );
    tampered.insert( "sample", sample );
    broken( QStringLiteral( "snapshot_sha256/path" ), tampered );
    // 估算被改（与按计划字段重算的值不符）。
    tampered = json;
    QJsonObject estimate = json.value( "resource_estimate" ).toObject();
    estimate.insert( "peak_bytes", 1.0 );
    tampered.insert( "resource_estimate", estimate );
    broken( QStringLiteral( "resource_estimate" ), tampered );
    // 单元身份被抹。
    tampered = json;
    tampered.insert( "unit", QJsonObject{} );
    broken( QStringLiteral( "unit-empty" ), tampered );
    tampered = json;
    tampered.insert( "unit", QJsonObject{ { "schemeId", QStringLiteral( "scheme" ) } } );
    broken( QStringLiteral( "unit" ), tampered );
    // 非整数网格/样本计数不得被 toInt() 截断。
    tampered = json;
    QJsonObject grid = json.value( "grid" ).toObject();
    grid.insert( "cols", 2.5 );
    tampered.insert( "grid", grid );
    broken( QStringLiteral( "fractional-cols" ), tampered );
    tampered = json;
    sample = json.value( "sample" ).toObject();
    sample.insert( "sample_count", 25.5 );
    tampered.insert( "sample", sample );
    broken( QStringLiteral( "fractional-sample-count" ), tampered );
    // 有效参数与变差/seed 投影不一致。
    tampered = json;
    QJsonObject effective = json.value( "effective_parameters" ).toObject();
    effective.insert( "RANGE", 50.0 );
    tampered.insert( "effective_parameters", effective );
    broken( QStringLiteral( "effective-variogram-mismatch" ), tampered );
    tampered = json;
    effective = json.value( "effective_parameters" ).toObject();
    effective.insert( "SEED", QStringLiteral( "7" ) );
    tampered.insert( "effective_parameters", effective );
    broken( QStringLiteral( "effective-seed-mismatch" ), tampered );
    // 冻结单元必须带具体修订。
    tampered = json;
    QJsonObject unit = json.value( "unit" ).toObject();
    unit.insert( "revision", QString() );
    tampered.insert( "unit", unit );
    broken( QStringLiteral( "unit-revision-empty" ), tampered );
    // 变差/网格越界。
    tampered = json;
    QJsonObject variogram = json.value( "variogram" ).toObject();
    variogram.insert( "sill", 0.0 );
    tampered.insert( "variogram", variogram );
    broken( QStringLiteral( "variogram" ), tampered );
    tampered = json;
    grid = json.value( "grid" ).toObject();
    grid.insert( "pixel_height", 10.0 );
    tampered.insert( "grid", grid );
    broken( QStringLiteral( "grid" ), tampered );
    // 种子被换成非法值/缺 schema。
    tampered = json;
    tampered.insert( "seed", QStringLiteral( "1e3" ) );
    broken( QStringLiteral( "seed" ), tampered );
    tampered = json;
    tampered.insert( "schema", QStringLiteral( "paleo.geostat-atom.v2" ) );
    broken( QStringLiteral( "schema" ), tampered );
    QVERIFY( GeostatFrozenPlan::fromJson( QJsonObject{}, &ok ).schema == plan.schema && !ok );
  }

  void toolSchemasMatchContract()
  {
    const QVector<AiToolSpec> specs = builtinAiToolSpecs();
    const AiToolSpec *kriging = nullptr;
    const AiToolSpec *sgs = nullptr;
    for( const auto &spec : specs )
    {
      if( spec.name == kKrigingToolAlias() )
        kriging = &spec;
      if( spec.name == kSgsToolAlias() )
        sgs = &spec;
    }
    QVERIFY( kriging && sgs );
    QVERIFY( isAiCapabilityTool( kKrigingToolAlias() ) );
    QVERIFY( isAiCapabilityTool( kSgsToolAlias() ) );
    QVERIFY( toolIsRouted( kKrigingToolAlias() ) );
    QVERIFY( toolIsRouted( kSgsToolAlias() ) );
    QCOMPARE( kriging->effectsForIntent( QStringLiteral( "publish" ) ),
              unsigned( paleo::host::Read | paleo::host::PrivateStage | paleo::host::CatalogPublish |
                        paleo::host::Presentation ) );
    QCOMPARE( kriging->effectsForIntent( QStringLiteral( "prepare" ) ),
              unsigned( paleo::host::Read | paleo::host::PrivateStage ) );

    const QJsonObject schema = kriging->toJsonSchema();
    QCOMPARE( schema.value( "additionalProperties" ), QJsonValue( false ) );
    const QJsonObject top = schema.value( "properties" ).toObject();
    const QJsonObject parameters = top.value( "parameters" ).toObject();
    const QJsonObject parameterProperties = parameters.value( "properties" ).toObject();
    QCOMPARE( parameters.value( "additionalProperties" ), QJsonValue( false ) );
    // 参数 schema 与解码的严格不等式同义：sill/range > 0、azimuth < 360、nugget ≥ 0。
    QCOMPARE( parameterProperties.value( "SILL" ).toObject().value( "exclusiveMinimum" ).toDouble(), 0.0 );
    QVERIFY( !parameterProperties.value( "SILL" ).toObject().contains( "minimum" ) );
    QCOMPARE( parameterProperties.value( "RANGE" ).toObject().value( "exclusiveMinimum" ).toDouble(), 0.0 );
    QCOMPARE( parameterProperties.value( "AZIMUTH_DEG" ).toObject().value( "exclusiveMaximum" ).toDouble(), 360.0 );
    QVERIFY( !parameterProperties.value( "AZIMUTH_DEG" ).toObject().contains( "maximum" ) );
    QCOMPARE( parameterProperties.value( "NUGGET" ).toObject().value( "minimum" ).toDouble(), 0.0 );
    QCOMPARE( parameterProperties.value( "MAX_POINTS" ).toObject().value( "maximum" ).toInt(), 64 );
    QVERIFY( parameterProperties.contains( "MIN_POINTS" ) );
    // 参数键集合 == v2 AlgorithmSpec 参数键（单一来源，不漂移）。
    QStringList specKeys, schemaKeys = parameterProperties.keys();
    for( const auto &field : AlgorithmSpecRegistry::specAtVersion( kKrigingAlgorithmId(), 2 ).params )
      specKeys.append( field.key );
    specKeys.sort();
    QCOMPARE( schemaKeys, specKeys );
    // 输入 schema：封闭、因子枚举与准入清单同源、extent 不设正负下限。
    const QJsonObject inputs = top.value( "inputs" ).toObject();
    const QJsonObject inputProperties = inputs.value( "properties" ).toObject();
    QCOMPARE( inputs.value( "additionalProperties" ), QJsonValue( false ) );
    QStringList expectedInputs{ "sample_ref", "factor_id", "field", "unit_selection", "grid", "source_operation_id" };
    expectedInputs.sort();
    QCOMPARE( inputProperties.keys(), expectedInputs );
    QStringList factorIds;
    for( const auto &value : inputProperties.value( "factor_id" ).toObject().value( "enum" ).toArray() )
      factorIds.append( value.toString() );
    QStringList contractIds;
    for( const GeostatFactorContract &factor : geostatFactorContracts() )
      contractIds.append( factor.id );
    QCOMPARE( factorIds, contractIds );
    const QJsonObject grid = inputProperties.value( "grid" ).toObject();
    QCOMPARE( grid.value( "additionalProperties" ), QJsonValue( false ) );
    for( const auto &key : { QStringLiteral( "min_x" ), QStringLiteral( "max_x" ),
                             QStringLiteral( "min_y" ), QStringLiteral( "max_y" ) } )
      QVERIFY2( !grid.value( "properties" ).toObject().value( key ).toObject().contains( "minimum" ),
                qPrintable( key ) );
    QCOMPARE( grid.value( "properties" ).toObject().value( "cell_size" ).toObject()
                  .value( "exclusiveMinimum" ).toDouble(), 0.0 );
    const QJsonObject unit = inputProperties.value( "unit_selection" ).toObject();
    QCOMPARE( unit.value( "additionalProperties" ), QJsonValue( false ) );
    QCOMPARE( unit.value( "required" ).toArray().size(), qsizetype( 3 ) );
    // SGS：REALIZATIONS/SEED 在，MIN_POINTS 不在；词表与契约同源。
    const QJsonObject sgsParameters =
        sgs->toJsonSchema().value( "properties" ).toObject().value( "parameters" ).toObject();
    const QJsonObject sgsProperties = sgsParameters.value( "properties" ).toObject();
    QVERIFY( sgsProperties.contains( "REALIZATIONS" ) );
    QCOMPARE( sgsProperties.value( "SEED" ).toObject().value( "type" ).toString(),
              QStringLiteral( "string" ) );
    QVERIFY( !sgsProperties.contains( "MIN_POINTS" ) );
    QStringList sgsSpecKeys, sgsSchemaKeys = sgsProperties.keys();
    for( const auto &field : AlgorithmSpecRegistry::specAtVersion( kSgsAlgorithmId(), 2 ).params )
      sgsSpecKeys.append( field.key );
    sgsSpecKeys.sort();
    QCOMPARE( sgsSchemaKeys, sgsSpecKeys );
    const QJsonArray models =
        parameterProperties.value( "VARIOGRAM_MODEL" ).toObject().value( "enum" ).toArray();
    QCOMPARE( models.size(), variogramModelVocabulary().size() );
    // 描述如实标注范围与语义。
    QVERIFY( kriging->description.contains( QStringLiteral( "无约束" ) ) );
    QVERIFY( sgs->description.contains( QStringLiteral( "不是克里金方差" ) ) );
    QVERIFY( sgs->description.contains( QStringLiteral( "MEMBER_n" ) ) );
    // 信封实参通过两档校验（嵌套严格校验在服务解码层）。
    QJsonObject arguments;
    arguments.insert( "client_request_id", QStringLiteral( "req-1" ) );
    arguments.insert( "binding_mode", QStringLiteral( "pinned-value" ) );
    arguments.insert( "publish_intent", QStringLiteral( "compute-only" ) );
    arguments.insert( "parameters", QJsonObject{ { "RANGE", 40.0 } } );
    arguments.insert( "inputs", QJsonObject{} );
    QVERIFY2( kriging->validateParameters( arguments ).isEmpty(),
              qPrintable( kriging->validateParameters( arguments ) ) );
    QVERIFY2( kriging->validateExternalArguments( arguments ).isEmpty(),
              qPrintable( kriging->validateExternalArguments( arguments ) ) );
    // Pi 宿主请求号绑定按 schema 的 client_request_id 自动纳入（无第二份名单）。
    QVERIFY( kriging->toJsonSchema().value( "properties" ).toObject().contains( "client_request_id" ) );
  }

  void planFactsAreBoundedAndPathFree()
  {
    GeostatDecodedRequest decoded;
    QCOMPARE( decode( GeostatMethod::Kriging, krigingParams(), baseInputs(), "prepare", &decoded ),
              QString() );
    const GeostatFrozenPlan plan = frozenPlanFrom( decoded, 25 );
    const QJsonObject facts = geostatPlanFacts( plan );
    const QByteArray bytes = QJsonDocument( facts ).toJson( QJsonDocument::Compact );
    QVERIFY( bytes.size() < 4096 ); // 有界：标量事实，无数组/路径/WKT
    const QString text = QString::fromUtf8( bytes );
    QVERIFY( !text.contains( QStringLiteral( "snapshotPath" ) ) );
    QVERIFY( !text.contains( QStringLiteral( ".paleo/" ) ) );
    QVERIFY( !text.contains( QStringLiteral( "sourceVerification" ) ) );
    const QJsonObject sample = facts.value( "sample" ).toObject();
    QCOMPARE( sample.value( "count" ).toInt(), 25 );
    QCOMPARE( sample.value( "skippedRows" ).toInt(), 2 );
    QCOMPARE( sample.value( "valueUnit" ).toString(), QStringLiteral( "m" ) );
    QCOMPARE( sample.value( "factorId" ).toString(), QStringLiteral( "sandthick" ) );
    QCOMPARE( facts.value( "constraintsApplied" ).toString(), QStringLiteral( "none" ) );
    QVERIFY( facts.value( "resourceEstimate" ).toObject().value( "peak_bytes" ).toDouble() > 0 );
    QCOMPARE( facts.value( "unit" ).toObject().value( "subIntervalId" ).toString(),
              QStringLiteral( "S1" ) );
    QVERIFY( !facts.contains( "seed" ) ); // 克里金无种子
    // 克里金计划里不应出现 SGS 键；SGS 计划带精确种子字符串（见 seed 用例）。
  }

private:
  static bool toolIsRouted( const QString &name)
  {
    QJsonObject arguments;
    arguments.insert( "client_request_id", QStringLiteral( "req-1" ) );
    arguments.insert( "binding_mode", QStringLiteral( "pinned-value" ) );
    arguments.insert( "parameters", QJsonObject{} );
    arguments.insert( "inputs", QJsonObject{} );
    const AiToolDispatch dispatch = dispatchAiTool( name, arguments );
    return dispatch.status == AiToolDispatchStatus::Routed;
  }
};

QTEST_MAIN(GeostatAtomContractTests)
#include "tst_geostat_atom_contract.moc"
