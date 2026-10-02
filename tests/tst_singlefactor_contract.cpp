// 层：数据（测试壳位于 tests/，被测对象为单因素消费边界与参数指纹）
#include <QtTest/QtTest>

#include "domain/singlefactorrequest.h"

#include <cmath>
#include <limits>

class SingleFactorContractTests : public QObject
{
  Q_OBJECT
private slots:
  void legacyRolesStayDistinct();
  void cartographicWorkIsNotQuantitative();
  void cartographicProductLayerIsSeparate();
  void parameterHashIsCanonical();
};

void SingleFactorContractTests::legacyRolesStayDistinct()
{
  using paleo::singlefactor::LegacyConstraintRole;
  using paleo::singlefactor::legacyConstraintRole;
  QCOMPARE( static_cast<int>( legacyConstraintRole( u"break_line" ) ),
            static_cast<int>( LegacyConstraintRole::HardBarrier ) );
  QCOMPARE( static_cast<int>( legacyConstraintRole( u"direction_line" ) ),
            static_cast<int>( LegacyConstraintRole::DirectionGuide ) );
  QCOMPARE( static_cast<int>( legacyConstraintRole( QStringView() ) ),
            static_cast<int>( LegacyConstraintRole::HullClip ) );
  QCOMPARE( static_cast<int>( legacyConstraintRole( u"line" ) ),
            static_cast<int>( LegacyConstraintRole::HullClip ) );
  QCOMPARE( static_cast<int>( legacyConstraintRole( u"interpretive_boundary" ) ),
            static_cast<int>( LegacyConstraintRole::NotInLegacyEngine ) );
  QCOMPARE( static_cast<int>( legacyConstraintRole( u"contour_stop" ) ),
            static_cast<int>( LegacyConstraintRole::NotInLegacyEngine ) );
  QCOMPARE( static_cast<int>( legacyConstraintRole( u"cartographic_detour" ) ),
            static_cast<int>( LegacyConstraintRole::NotInLegacyEngine ) );
  QCOMPARE( static_cast<int>( legacyConstraintRole( u"hard_barrier" ) ),
            static_cast<int>( LegacyConstraintRole::HullClip ) );
}

void SingleFactorContractTests::cartographicWorkIsNotQuantitative()
{
  using paleo::singlefactor::isAnalysisFactorRaster;
  using paleo::singlefactor::rejectsQuantitativeUse;
  QVERIFY( isAnalysisFactorRaster( u"single_factor_raster", QStringView() ) );
  QVERIFY( isAnalysisFactorRaster( u"single_factor_raster", u"analysis" ) );
  QVERIFY( !rejectsQuantitativeUse( u"single_factor_raster", QStringView() ) );
  QVERIFY( rejectsQuantitativeUse( u"single_factor_raster", u"cartographic_work" ) );
  QVERIFY( !isAnalysisFactorRaster( u"single_factor_raster", u"cartographic_work" ) );
  QVERIFY( rejectsQuantitativeUse( u"single_factor_cartographic_work", QStringView() ) );
  QVERIFY( rejectsQuantitativeUse( u"single_factor_cartographic_contour", u"cartographic_work" ) );
  QVERIFY( !rejectsQuantitativeUse( u"facies_raster", QStringView() ) );
}

void SingleFactorContractTests::cartographicProductLayerIsSeparate()
{
  using paleo::singlefactor::isCartographicProductLayer;
  QVERIFY( !isCartographicProductLayer( u"factor.D61.sandthick", u"04_SingleFactor" ) );
  QVERIFY( !isCartographicProductLayer( u"contours.D61.sandthick", u"04_SingleFactor/Contours" ) );
  QVERIFY( isCartographicProductLayer( u"cartographic.D61.sandthick", u"04_SingleFactor/Cartographic" ) );
  QVERIFY( isCartographicProductLayer( u"factor.D61.sandthick", u"04_SingleFactor/Cartographic" ) );
  QVERIFY( !isCartographicProductLayer( u"composite.D61", u"05_PaleoMap" ) );
}

void SingleFactorContractTests::parameterHashIsCanonical()
{
  QVariantMap first{{QStringLiteral( "b" ), 2}, {QStringLiteral( "a" ), QStringLiteral( "x" )}};
  QVariantMap second{{QStringLiteral( "a" ), QStringLiteral( "x" )}, {QStringLiteral( "b" ), 2.0}};
  const auto left = paleo::singlefactor::parameterHash( first );
  const auto right = paleo::singlefactor::parameterHash( second );
  QVERIFY( left.ok );
  QVERIFY( right.ok );
  QCOMPARE( left.canonical, right.canonical );
  QCOMPARE( left.sha256, right.sha256 );
  QCOMPARE( left.sha256.size(), 64 );

  QVariantMap reorderedArray = first;
  reorderedArray.insert( QStringLiteral( "levels" ), QVariantList{ 1.0, 2.0 } );
  QVariantMap swappedArray = first;
  swappedArray.insert( QStringLiteral( "levels" ), QVariantList{ 2.0, 1.0 } );
  const auto levels = paleo::singlefactor::parameterHash( reorderedArray );
  const auto swapped = paleo::singlefactor::parameterHash( swappedArray );
  QVERIFY( levels.ok );
  QVERIFY( levels.sha256 != swapped.sha256 );

  QVariantMap poisoned = first;
  poisoned.insert( QStringLiteral( "power" ), std::numeric_limits<double>::quiet_NaN() );
  const auto rejected = paleo::singlefactor::parameterHash( poisoned );
  QVERIFY( !rejected.ok );
  QVERIFY( rejected.sha256.isEmpty() );
}

QTEST_MAIN( SingleFactorContractTests )
#include "tst_singlefactor_contract.moc"
