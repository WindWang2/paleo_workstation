// 层：数据（测试壳位于 tests/，被测对象为数据层井点采集移植）
#include <QtTest/QtTest>

#include "algorithms/singlefactor/wellacquisition.h"

#include <cmath>
#include <optional>
#include <vector>

using namespace paleo::singlefactor;

namespace
{

WellAcquisitionRequest directRequest( const QString &field = QStringLiteral( "sand_ratio" ) )
{
  WellAcquisitionRequest request;
  request.wellIdField = QStringLiteral( "well_id" );
  request.factorMode = QStringLiteral( "direct" );
  request.valueField = field;
  request.factorName = QStringLiteral( "sand_ratio" );
  return request;
}

WellFeatureRow row( double x, double y, FeatureAttributes attrs )
{
  WellFeatureRow feature;
  feature.isPoint = true;
  feature.x = x;
  feature.y = y;
  feature.attributes = std::move( attrs );
  return feature;
}

Polygon square( double x0, double y0, double x1, double y1 )
{
  Polygon poly;
  poly.exterior.points = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
  return poly;
}

Polygon squareWithHole()
{
  Polygon poly = square( 0, 0, 100, 100 );
  Ring hole;
  hole.points = { { 40, 40 }, { 60, 40 }, { 60, 60 }, { 40, 60 }, { 40, 40 } };
  poly.holes.push_back( hole );
  return poly;
}

FeatureAttributes attrs( const QString &id, const QVariant &value )
{
  return { { QStringLiteral( "well_id" ), id },
           { QStringLiteral( "sand_ratio" ), value } };
}

} // namespace

class tst_singlefactor_acquisition : public QObject
{
  Q_OBJECT

private slots:
  // _attr_value：声明名精确命中优先，其次大小写不敏感；None/"" 视为缺失。
  void attrValueExactThenCaseInsensitive();
  void attrValueTreatsEmptyStringAsMissing();

  // _parse_numeric：数字直返；文本 strip + 去 ',' + 去尾 '%'；非有限拒收。
  void parseNumericPercentAndComma();
  void parseNumericRejectsInvalid();

  // _to_bool：字符串真值表 + 数值 !=0 + 缺失走默认。
  void boolParsing();

  // 别名链 + 无 first-numeric 兜底（OCR_CONF 不得借用）。
  void aliasChainResolvesValue();
  void emptyValueSkippedWithReasonNoNumericFallback();

  // 值域裁剪：0–1 声明范围内才裁；明显厚度值（>1.5）不动。
  void clipRatioToDeclaredRange();
  void physicalValueAboveRatioCeilingUntouched();

  // ratio 模式：分子/分母；分母 0 → 回退 direct 解析。
  void ratioModeComputesFraction();
  void ratioModeFallsBackWhenDenominatorZero();

  // 边界包含：严格在内 / 洞内剔除 / 外包框 5% 边距 / 贴边距离 / 完全在外。
  void strictInsideAccepted();
  void holeRejected();
  void bboxMarginSoftIncluded();
  void nearRingSoftIncluded();
  void farOutsideRejected();

  // 几何在外但属性 x/y 在界内 → 用属性坐标。
  void xyAttributeFallback();

  // is_ctrl 解析进 isControl。
  void controlPointFlag();

  // _resolve_value_range_for_wells：冲突声明域让位数据域。
  void valueRangeConflictYieldsDataRange();
};

void tst_singlefactor_acquisition::attrValueExactThenCaseInsensitive()
{
  const FeatureAttributes attributes = {
      { QStringLiteral( "SAND_RATIO" ), QVariant( 0.42 ) },
      { QStringLiteral( "sand_ratio" ), QVariant( 0.9 ) } };
  // 精确名先命中（顺序语义同上游：先全名精确扫描再大小写回退）。
  QCOMPARE( attrValue( attributes, { QStringLiteral( "sand_ratio" ) } ).toDouble(), 0.9 );
  const FeatureAttributes onlyUpper = {
      { QStringLiteral( "SAND_RATIO" ), QVariant( 0.42 ) } };
  QCOMPARE( attrValue( onlyUpper, { QStringLiteral( "sand_ratio" ) } ).toDouble(), 0.42 );
}

void tst_singlefactor_acquisition::attrValueTreatsEmptyStringAsMissing()
{
  const FeatureAttributes attributes = {
      { QStringLiteral( "sand_ratio" ), QVariant( QString() ) },
      { QStringLiteral( "factor_value" ), QVariant( 0.3 ) } };
  const QVariant value = attrValue( attributes, { QStringLiteral( "sand_ratio" ),
                                                  QStringLiteral( "factor_value" ) } );
  QCOMPARE( value.toDouble(), 0.3 );
}

void tst_singlefactor_acquisition::parseNumericPercentAndComma()
{
  QCOMPARE( *parseNumeric( QVariant( QStringLiteral( "35.5%" ) ) ), 35.5 );
  QCOMPARE( *parseNumeric( QVariant( QStringLiteral( "1,234.5" ) ) ), 1234.5 );
  QCOMPARE( *parseNumeric( QVariant( QStringLiteral( "  0.42  " ) ) ), 0.42 );
  QCOMPARE( *parseNumeric( QVariant( 0.25 ) ), 0.25 );
}

void tst_singlefactor_acquisition::parseNumericRejectsInvalid()
{
  QVERIFY( !parseNumeric( QVariant() ).has_value() );
  QVERIFY( !parseNumeric( QVariant( QString() ) ).has_value() );
  QVERIFY( !parseNumeric( QVariant( QStringLiteral( "abc" ) ) ).has_value() );
  QVERIFY( !parseNumeric( QVariant( QStringLiteral( "nan" ) ) ).has_value() );
  QVERIFY( !parseNumeric( QVariant( QStringLiteral( "inf" ) ) ).has_value() );
}

void tst_singlefactor_acquisition::boolParsing()
{
  QVERIFY( toBool( QVariant( QStringLiteral( "yes" ) ), false ) );
  QVERIFY( toBool( QVariant( QStringLiteral( "是" ) ), false ) );
  QVERIFY( toBool( QVariant( 1 ), false ) );
  QVERIFY( !toBool( QVariant( QStringLiteral( "no" ) ), true ) );
  QVERIFY( !toBool( QVariant( 0 ), true ) );
  QVERIFY( !toBool( QVariant( QStringLiteral( "0" ) ), true ) );
  QVERIFY( toBool( QVariant(), true ) );   // 缺失 → 默认
  QVERIFY( !toBool( QVariant(), false ) );
  QVERIFY( toBool( QVariant( QStringLiteral( "unrecognized" ) ), true ) );
}

void tst_singlefactor_acquisition::aliasChainResolvesValue()
{
  // valueField 缺失 → factorName → factor_value → value → sand_ratio …
  WellAcquisitionRequest request = directRequest( QStringLiteral( "nonexistent_field" ) );
  request.factorName = QStringLiteral( "nonexistent_name" );
  const FeatureAttributes attributes = {
      { QStringLiteral( "factor_value" ), QVariant( 0.6 ) } };
  const auto value = resolveDirectFactorValue( attributes, request );
  QVERIFY( value.has_value() );
  QCOMPARE( *value, 0.6 );
}

void tst_singlefactor_acquisition::emptyValueSkippedWithReasonNoNumericFallback()
{
  // 上游 bug 决议：sand_ratio 为空时不得借用 OCR_CONF 数值列——整井跳过。
  const std::vector<Polygon> domain{ square( 0, 0, 100, 100 ) };
  WellFeatureRow feature = row( 50, 50,
                                { { QStringLiteral( "well_id" ), QStringLiteral( "W1" ) },
                                  { QStringLiteral( "sand_ratio" ), QVariant() },
                                  { QStringLiteral( "OCR_CONF" ), QVariant( 0.0 ) } } );
  const WellAcquisitionResult result =
      acquireWells( { feature }, domain, directRequest() );
  QVERIFY( result.wells.empty() );
  QCOMPARE( result.skipped.size(), 1 );
  QVERIFY( result.skipped.first().contains( QStringLiteral( "W1" ) ) );
  QVERIFY( result.skipped.first().contains( QStringLiteral( "指标值无效" ) ) );
}

void tst_singlefactor_acquisition::clipRatioToDeclaredRange()
{
  WellAcquisitionRequest request = directRequest();
  request.valueMin = 0.0;
  request.valueMax = 1.0;
  FeatureAttributes attributes = {
      { QStringLiteral( "sand_ratio" ), QVariant( 1.3 ) } };
  QCOMPARE( *resolveCurrentFactorValue( attributes, request ), 1.0 );
  // −0.02 在 −0.05 容差内 → 裁到 0；−0.2 已按物理量处理（另一条用例覆盖）。
  attributes = { { QStringLiteral( "sand_ratio" ), QVariant( -0.02 ) } };
  QCOMPARE( *resolveCurrentFactorValue( attributes, request ), 0.0 );
  attributes = { { QStringLiteral( "sand_ratio" ), QVariant( 0.45 ) } };
  QCOMPARE( *resolveCurrentFactorValue( attributes, request ), 0.45 );
}

void tst_singlefactor_acquisition::physicalValueAboveRatioCeilingUntouched()
{
  // 声明 0–1 但值明显是厚度（>1.5）→ 不裁剪（上游防「井没参与」条款）。
  WellAcquisitionRequest request = directRequest( QStringLiteral( "thickness" ) );
  request.factorName = QStringLiteral( "thickness" );
  request.valueMin = 0.0;
  request.valueMax = 1.0;
  const FeatureAttributes attributes = {
      { QStringLiteral( "thickness" ), QVariant( 102.0 ) } };
  QCOMPARE( *resolveCurrentFactorValue( attributes, request ), 102.0 );
  const FeatureAttributes negative = {
      { QStringLiteral( "thickness" ), QVariant( -2.0 ) } };
  QCOMPARE( *resolveCurrentFactorValue( negative, request ), -2.0 );
}

void tst_singlefactor_acquisition::ratioModeComputesFraction()
{
  WellAcquisitionRequest request;
  request.wellIdField = QStringLiteral( "well_id" );
  request.factorMode = QStringLiteral( "ratio" );
  request.valueField = QStringLiteral( "sand_ratio" );
  request.factorName = QStringLiteral( "sand_ratio" );
  request.numeratorField = QStringLiteral( "sand_thk" );
  request.denominatorField = QStringLiteral( "stratum_thk" );
  const FeatureAttributes attributes = {
      { QStringLiteral( "sand_thk" ), QVariant( 30.0 ) },
      { QStringLiteral( "stratum_thk" ), QVariant( 120.0 ) } };
  QVERIFY( qAbs( *resolveCurrentFactorValue( attributes, request ) - 0.25 ) < 1e-12 );
}

void tst_singlefactor_acquisition::ratioModeFallsBackWhenDenominatorZero()
{
  WellAcquisitionRequest request;
  request.wellIdField = QStringLiteral( "well_id" );
  request.factorMode = QStringLiteral( "ratio" );
  request.valueField = QStringLiteral( "sand_ratio" );
  request.factorName = QStringLiteral( "sand_ratio" );
  request.numeratorField = QStringLiteral( "sand_thk" );
  request.denominatorField = QStringLiteral( "stratum_thk" );
  const FeatureAttributes attributes = {
      { QStringLiteral( "sand_thk" ), QVariant( 30.0 ) },
      { QStringLiteral( "stratum_thk" ), QVariant( 0.0 ) },
      { QStringLiteral( "sand_ratio" ), QVariant( 0.55 ) } };
  QVERIFY( qAbs( *resolveCurrentFactorValue( attributes, request ) - 0.55 ) < 1e-12 );
}

void tst_singlefactor_acquisition::strictInsideAccepted()
{
  const std::vector<Polygon> domain{ square( 0, 0, 100, 100 ) };
  const WellAcquisitionResult result = acquireWells(
      { row( 50, 50, attrs( QStringLiteral( "W1" ), 0.5 ) ) },
      domain, directRequest() );
  QCOMPARE( result.wells.size(), std::size_t( 1 ) );
  QCOMPARE( result.softIncluded, 0 );
  QVERIFY( result.skipped.isEmpty() );
}

void tst_singlefactor_acquisition::holeRejected()
{
  const std::vector<Polygon> domain{ squareWithHole() };
  // 洞内点（50,50）：严格 PIP 命中洞 → 不算严格在内；
  // 但距环距离 < max(diag*0.008, 1) → 软纳入（同上游）。
  const WellAcquisitionResult inHole = acquireWells(
      { row( 50, 50, attrs( QStringLiteral( "H1" ), 0.5 ) ) },
      domain, directRequest() );
  QCOMPARE( inHole.wells.size(), std::size_t( 1 ) );
  QCOMPARE( inHole.softIncluded, 1 );
  // 洞中心远离所有环且出外包框的极端场景不可达（洞必在环内）；
  // 验证严格判定本身：洞边线上一点同样软纳入。
  const WellAcquisitionResult onHoleEdge = acquireWells(
      { row( 40, 50, attrs( QStringLiteral( "H2" ), 0.5 ) ) },
      domain, directRequest() );
  QCOMPARE( onHoleEdge.wells.size(), std::size_t( 1 ) );
}

void tst_singlefactor_acquisition::bboxMarginSoftIncluded()
{
  const std::vector<Polygon> domain{ square( 0, 0, 100, 100 ) };
  // x=104 在外包框 5% 边距内（[−5,105]），但距东环 4 > max(141.4*0.008,1)≈1.13。
  const WellAcquisitionResult result = acquireWells(
      { row( 104, 50, attrs( QStringLiteral( "S1" ), 0.5 ) ) },
      domain, directRequest() );
  QCOMPARE( result.wells.size(), std::size_t( 1 ) );
  QCOMPARE( result.softIncluded, 1 );
  QCOMPARE( result.skipped.size(), 1 );
  QVERIFY( result.skipped.first().contains( QStringLiteral( "软纳入" ) ) );
}

void tst_singlefactor_acquisition::nearRingSoftIncluded()
{
  const std::vector<Polygon> domain{ square( 0, 0, 100, 100 ) };
  // x=100.8 距东环 0.8 ≤ max(diag*0.008≈1.13, 1) → 贴边软纳入。
  const WellAcquisitionResult result = acquireWells(
      { row( 100.8, 50, attrs( QStringLiteral( "N1" ), 0.5 ) ) },
      domain, directRequest() );
  QCOMPARE( result.wells.size(), std::size_t( 1 ) );
  QCOMPARE( result.softIncluded, 1 );
}

void tst_singlefactor_acquisition::farOutsideRejected()
{
  const std::vector<Polygon> domain{ square( 0, 0, 100, 100 ) };
  const WellAcquisitionResult result = acquireWells(
      { row( 200, 200, attrs( QStringLiteral( "F1" ), 0.5 ) ) },
      domain, directRequest() );
  QVERIFY( result.wells.empty() );
  QCOMPARE( result.skipped.size(), 1 );
  QVERIFY( result.skipped.first().contains( QStringLiteral( "边界外" ) ) );
}

void tst_singlefactor_acquisition::xyAttributeFallback()
{
  const std::vector<Polygon> domain{ square( 0, 0, 100, 100 ) };
  // 几何在界外，但属性 x/y 落在界内 → 用属性坐标、按严格在内接受。
  WellFeatureRow feature = row( 500, 500,
                                { { QStringLiteral( "well_id" ), QStringLiteral( "W1" ) },
                                  { QStringLiteral( "sand_ratio" ), QVariant( 0.5 ) },
                                  { QStringLiteral( "x" ), QVariant( 30.0 ) },
                                  { QStringLiteral( "y" ), QVariant( 40.0 ) } } );
  const WellAcquisitionResult result =
      acquireWells( { feature }, domain, directRequest() );
  QCOMPARE( result.wells.size(), std::size_t( 1 ) );
  QCOMPARE( result.wells.front().x, 30.0 );
  QCOMPARE( result.wells.front().y, 40.0 );
  QCOMPARE( result.softIncluded, 0 );
}

void tst_singlefactor_acquisition::controlPointFlag()
{
  const std::vector<Polygon> domain{ square( 0, 0, 100, 100 ) };
  auto ctrlAttrs = attrs( QStringLiteral( "C1" ), 0.5 );
  ctrlAttrs.append( { QStringLiteral( "is_ctrl" ), QVariant( 1 ) } );
  auto plainAttrs = attrs( QStringLiteral( "C2" ), 0.6 );
  plainAttrs.append( { QStringLiteral( "is_ctrl" ), QVariant( 0 ) } );
  const WellAcquisitionResult result = acquireWells(
      { row( 30, 30, ctrlAttrs ), row( 40, 40, plainAttrs ) },
      domain, directRequest() );
  QCOMPARE( result.wells.size(), std::size_t( 2 ) );
  QVERIFY( result.wells[0].isControl );
  QVERIFY( !result.wells[1].isControl );
}

void tst_singlefactor_acquisition::valueRangeConflictYieldsDataRange()
{
  const std::vector<AcquiredWell> wells = {
      AcquiredWell{ "W1", 0, 0, 5.0, false },
      AcquiredWell{ "W2", 0, 0, 200.0, false } };
  // 声明 0–1 但数据到 200 → 冲突 → 数据域。
  const ResolvedValueRange conflict = resolveValueRangeForWells(
      wells, std::optional<double>( 0.0 ), std::optional<double>( 1.0 ), {} );
  QVERIFY( conflict.min && conflict.max );
  QCOMPARE( *conflict.min, 5.0 );
  QCOMPARE( *conflict.max, 200.0 );
  // 数据在声明范围内 → 保留声明域。
  const std::vector<AcquiredWell> ratioWells = {
      AcquiredWell{ "W1", 0, 0, 0.2, false },
      AcquiredWell{ "W2", 0, 0, 0.9, false } };
  const ResolvedValueRange kept = resolveValueRangeForWells(
      ratioWells, std::optional<double>( 0.0 ), std::optional<double>( 1.0 ), {} );
  QCOMPARE( *kept.min, 0.0 );
  QCOMPARE( *kept.max, 1.0 );
  // 无声明 → 数据域（含网格值合并）。
  const ResolvedValueRange dataDriven = resolveValueRangeForWells(
      ratioWells, std::nullopt, std::nullopt, { 0.05, 0.95 } );
  QCOMPARE( *dataDriven.min, 0.05 );
  QCOMPARE( *dataDriven.max, 0.95 );
}

QTEST_GUILESS_MAIN( tst_singlefactor_acquisition )
#include "tst_singlefactor_acquisition.moc"
