// 层：数据（测试壳位于 tests/，被测对象为变差函数核）
#include <QtTest/QtTest>

#include "algorithms/geostat/variogram.h"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

using namespace paleo::geostat;

namespace
{

// 确定性 (0,1) 均匀：mt19937_64 原始 u64 → 53bit 尾数（跨平台位稳定）。
double unitFromU64( std::uint64_t bits )
{
  return ( static_cast<double>( bits >> 11 ) + 0.5 ) * 0x1.0p-53;
}

// 各向异性高斯卷积场：白噪声（固定 seed）⊗ 可分离高斯核（σx 短、σy 长）。
// 相关函数 ρ(Δx,Δy) ∝ exp(−Δx²/4σx² − Δy²/4σy²)，单调不振荡——
// 变差函数单调升到基台，是理论模型拟合的合理被测场。
// 平面波叠加场的相关是 Bessel 型振荡（γ 越过基台来回摆），不用于拟合断言。
class AnisotropicField
{
public:
  AnisotropicField( double extent, int fineCount, double sigmaX, double sigmaY, std::uint64_t seed )
  {
    const double margin = 4.0 * std::max( sigmaX, sigmaY );
    const int padded = fineCount + 2 * static_cast<int>( std::ceil( margin / ( extent / ( fineCount - 1 ) ) ) );
    m_spacing = extent / ( fineCount - 1 );
    m_origin = -margin;
    m_count = padded;

    std::vector<double> noise( static_cast<std::size_t>( padded ) * padded );
    std::mt19937_64 rng( seed );
    for ( std::size_t i = 0; i + 1 < noise.size(); i += 2 )
    {
      const double u1 = unitFromU64( rng() );
      const double u2 = unitFromU64( rng() );
      const double radius = std::sqrt( -2.0 * std::log( u1 ) );
      const double angle = 2.0 * std::numbers::pi * u2;
      noise[i] = radius * std::cos( angle );
      noise[i + 1] = radius * std::sin( angle );
    }
    // 可分离高斯平滑：先横后纵，边界反射填充
    const int tapsX = static_cast<int>( std::ceil( 4.0 * sigmaX / m_spacing ) );
    const int tapsY = static_cast<int>( std::ceil( 4.0 * sigmaY / m_spacing ) );
    std::vector<double> kernelX( static_cast<std::size_t>( 2 * tapsX ) + 1 );
    std::vector<double> kernelY( static_cast<std::size_t>( 2 * tapsY ) + 1 );
    fillGaussian( kernelX, tapsX, sigmaX, m_spacing );
    fillGaussian( kernelY, tapsY, sigmaY, m_spacing );

    std::vector<double> smoothed( noise.size(), 0.0 );
    for ( int row = 0; row < padded; ++row )
      convolveRow( noise, smoothed, row, padded, kernelX, tapsX );
    std::vector<double> vertical( noise.size(), 0.0 );
    for ( int column = 0; column < padded; ++column )
      convolveColumn( smoothed, vertical, column, padded, kernelY, tapsY );
    m_values = std::move( vertical );
  }

  double value( double x, double y ) const
  {
    const double fx = ( x - m_origin ) / m_spacing - 0.5;
    const double fy = ( y - m_origin ) / m_spacing - 0.5;
    const int x0 = std::clamp( static_cast<int>( std::floor( fx ) ), 0, m_count - 2 );
    const int y0 = std::clamp( static_cast<int>( std::floor( fy ) ), 0, m_count - 2 );
    const double tx = std::clamp( fx - x0, 0.0, 1.0 );
    const double ty = std::clamp( fy - y0, 0.0, 1.0 );
    const auto at = [this]( int column, int row ) {
      return m_values[static_cast<std::size_t>( row ) * m_count + column];
    };
    return ( at( x0, y0 ) * ( 1 - tx ) + at( x0 + 1, y0 ) * tx ) * ( 1 - ty ) +
           ( at( x0, y0 + 1 ) * ( 1 - tx ) + at( x0 + 1, y0 + 1 ) * tx ) * ty;
  }

private:
  static void fillGaussian( std::vector<double> &kernel, int taps, double sigma, double spacing )
  {
    double sum = 0;
    for ( int k = -taps; k <= taps; ++k )
    {
      const double d = k * spacing;
      kernel[static_cast<std::size_t>( k + taps )] = std::exp( -d * d / ( 2 * sigma * sigma ) );
      sum += kernel[static_cast<std::size_t>( k + taps )];
    }
    for ( double &weight : kernel )
      weight /= sum;
  }

  static void convolveRow( const std::vector<double> &input, std::vector<double> &output,
                           int row, int count, const std::vector<double> &kernel, int taps )
  {
    for ( int column = 0; column < count; ++column )
    {
      double sum = 0;
      for ( int k = -taps; k <= taps; ++k )
      {
        int source = column + k;
        if ( source < 0 )
          source = -source;
        if ( source >= count )
          source = 2 * ( count - 1 ) - source;
        sum += input[static_cast<std::size_t>( row ) * count + source] *
               kernel[static_cast<std::size_t>( k + taps )];
      }
      output[static_cast<std::size_t>( row ) * count + column] = sum;
    }
  }

  static void convolveColumn( const std::vector<double> &input, std::vector<double> &output,
                              int column, int count, const std::vector<double> &kernel, int taps )
  {
    for ( int row = 0; row < count; ++row )
    {
      double sum = 0;
      for ( int k = -taps; k <= taps; ++k )
      {
        int source = row + k;
        if ( source < 0 )
          source = -source;
        if ( source >= count )
          source = 2 * ( count - 1 ) - source;
        sum += input[static_cast<std::size_t>( source ) * count + column] *
               kernel[static_cast<std::size_t>( k + taps )];
      }
      output[static_cast<std::size_t>( row ) * count + column] = sum;
    }
  }

  std::vector<double> m_values;
  int m_count = 0;
  double m_spacing = 1;
  double m_origin = 0;
};

std::vector<Sample> sampleField( const AnisotropicField &field, int n, double spacing )
{
  std::vector<Sample> samples;
  samples.reserve( static_cast<std::size_t>( n * n ) );
  for ( int row = 0; row < n; ++row )
    for ( int column = 0; column < n; ++column )
    {
      const double x = column * spacing;
      const double y = row * spacing;
      samples.push_back( Sample{ x, y, field.value( x, y ) } );
    }
  return samples;
}

} // namespace

class GeostatVariogramTests : public QObject
{
  Q_OBJECT
private slots:
  void modelShapes();
  void constantFieldIsZeroEverywhere();
  void anisotropicFieldStrikeRangeExceedsDipRange();
  void fitResidualWithinThreshold();
  void invalidInputs();
};

void GeostatVariogramTests::modelShapes()
{
  VariogramModel model;
  model.type = VariogramModelType::Spherical;
  model.nugget = 1;
  model.sill = 4;
  model.range = 10;
  QCOMPARE( model.semivariance( 0 ), 0.0 );
  QCOMPARE( model.semivariance( 10 ), 5.0 ); // h = range → 总基台
  QCOMPARE( model.semivariance( 25 ), 5.0 ); // h > range 平台
  QVERIFY( model.semivariance( 5 ) > 1.0 && model.semivariance( 5 ) < 5.0 );

  model.type = VariogramModelType::Exponential;
  QVERIFY( std::fabs( model.semivariance( 10 ) - ( 1 + 4 * ( 1 - std::exp( -3.0 ) ) ) ) < 1e-12 );

  model.type = VariogramModelType::Gaussian;
  QVERIFY( std::fabs( model.semivariance( 10 ) - ( 1 + 4 * ( 1 - std::exp( -3.0 ) ) ) ) < 1e-12 );
  QVERIFY( model.semivariance( 100 ) > 4.99 ); // 平台

  // 几何各向异性：az=0（走向南北，长变程沿 y），ratio=2 → 垂直变程减半
  VariogramModel anisotropic = model;
  anisotropic.type = VariogramModelType::Spherical;
  anisotropic.anisotropyRatio = 2;
  anisotropic.azimuthDeg = 0;
  QCOMPARE( anisotropic.semivariance( 0.0, 10.0 ), 5.0 ); // 沿走向 h=range → 总基台
  QCOMPARE( anisotropic.semivariance( 10.0, 0.0 ), 5.0 ); // 垂直 h_eff=2×10 > range → 总基台
  QVERIFY( anisotropic.semivariance( 4.9, 0.0 ) < 5.0 ); // 垂直方向变程 = range/2 = 5
  QCOMPARE( anisotropic.semivariance( 6.0, 0.0 ), 5.0 ); // 垂直 6 > 5 → 基台
  QVERIFY( anisotropic.semivariance( 0.0, 6.0 ) < 4.5 ); // 走向 6 < 10 → 仍在爬升
}

void GeostatVariogramTests::constantFieldIsZeroEverywhere()
{
  const std::vector<Sample> samples = sampleField(
      AnisotropicField( 200, 8, 1, 1, 7 ), 10, 20 ); // 场形状无关：下面直接钉常值
  std::vector<Sample> constant = samples;
  for ( Sample &sample : constant )
    sample.value = 7.5;
  const ExperimentalVariogram ev = experimentalVariogram( constant, 20, 8 );
  QCOMPARE( ev.status, Status::Ok );
  int lagsWithPairs = 0;
  for ( int k = 0; k < ev.nLags; ++k )
  {
    if ( ev.pairCount[static_cast<std::size_t>( k )] > 0 )
    {
      QCOMPARE( ev.semivariance[static_cast<std::size_t>( k )], 0.0 );
      ++lagsWithPairs;
    }
  }
  QVERIFY( lagsWithPairs >= 6 ); // 10×10 网格 8 个 lag 窗内至少 6 个有对
  QCOMPARE( ev.pairCount[0], 0 ); // 采样间距 = lag 时 bin0（h<lag）无对——口径如实

  const VariogramFit fit = fitVariogram( ev, VariogramModelType::Spherical );
  QCOMPARE( fit.status, Status::Ok );
  QVERIFY( fit.model.nugget + fit.model.sill == 0.0 ); // 零信号 → 零参数模型
}

void GeostatVariogramTests::anisotropicFieldStrikeRangeExceedsDipRange()
{
  // σx=6、σy=18 → 高斯实用变程约 x:20.8 / y:62.4（走向 = 南北，az=0 拉长）
  const AnisotropicField field( 400, 201, 6.0, 18.0, 20261003 );
  const std::vector<Sample> samples = sampleField( field, 41, 10 );
  VariogramDirection northSouth;
  northSouth.omnidirectional = false;
  northSouth.azimuthDeg = 0;
  northSouth.toleranceDeg = 22.5;
  VariogramDirection eastWest;
  eastWest.omnidirectional = false;
  eastWest.azimuthDeg = 90;
  eastWest.toleranceDeg = 22.5;

  const ExperimentalVariogram evStrike = experimentalVariogram( samples, 10, 12, northSouth );
  const ExperimentalVariogram evDip = experimentalVariogram( samples, 10, 12, eastWest );
  QCOMPARE( evStrike.status, Status::Ok );
  QCOMPARE( evDip.status, Status::Ok );

  const VariogramFit fitStrike = fitVariogram( evStrike, VariogramModelType::Spherical );
  const VariogramFit fitDip = fitVariogram( evDip, VariogramModelType::Spherical );
  QCOMPARE( fitStrike.status, Status::Ok );
  QCOMPARE( fitDip.status, Status::Ok );
  QVERIFY2( fitStrike.model.range > 1.5 * fitDip.model.range,
            qPrintable( QStringLiteral( "strike range=%1 dip range=%2" )
                            .arg( fitStrike.model.range )
                            .arg( fitDip.model.range ) ) );

  // 全向变差函数落两者之间（尺度混合的自洽检查）
  const ExperimentalVariogram evOmni = experimentalVariogram( samples, 10, 12 );
  const VariogramFit fitOmni = fitVariogram( evOmni, VariogramModelType::Spherical );
  QCOMPARE( fitOmni.status, Status::Ok );
  QVERIFY2( fitOmni.model.range > fitDip.model.range && fitOmni.model.range < fitStrike.model.range,
            qPrintable( QStringLiteral( "omni=%1 dip=%2 strike=%3" )
                            .arg( fitOmni.model.range )
                            .arg( fitDip.model.range )
                            .arg( fitStrike.model.range ) ) );
}

void GeostatVariogramTests::fitResidualWithinThreshold()
{
  const AnisotropicField field( 400, 201, 6.0, 18.0, 20261003 );
  const std::vector<Sample> samples = sampleField( field, 41, 10 );
  const ExperimentalVariogram ev = experimentalVariogram( samples, 10, 12 );
  QCOMPARE( ev.status, Status::Ok );

  // 高斯卷积场用高斯模型拟合：残差阈值断言（单调场 → 高 r2）
  const VariogramFit fit = fitVariogram( ev, VariogramModelType::Gaussian );
  QCOMPARE( fit.status, Status::Ok );
  const double totalSill = fit.model.nugget + fit.model.sill;
  QVERIFY2( totalSill > 0, "non-degenerate field must yield positive sill" );
  QVERIFY2( fit.r2 >= 0.9,
            qPrintable( QStringLiteral( "gaussian fit r2=%1" ).arg( fit.r2 ) ) );
  QVERIFY2( fit.rmse <= 0.08 * totalSill,
            qPrintable( QStringLiteral( "rmse=%1 totalSill=%2" ).arg( fit.rmse ).arg( totalSill ) ) );
}

void GeostatVariogramTests::invalidInputs()
{
  const std::vector<Sample> samples = sampleField( AnisotropicField( 100, 16, 3, 3, 1 ), 5, 20 );
  QVERIFY( experimentalVariogram( samples, 0, 8 ).status == Status::InvalidInput );
  QVERIFY( experimentalVariogram( samples, 20, 0 ).status == Status::InvalidInput );
  QVERIFY( experimentalVariogram( {}, 20, 8 ).status == Status::InvalidInput );
  const std::vector<Sample> single = { Sample{ 0, 0, 1 } };
  QVERIFY( experimentalVariogram( single, 20, 8 ).status == Status::InvalidInput );

  ExperimentalVariogram degenerate; // 无实验点
  QVERIFY( fitVariogram( degenerate, VariogramModelType::Spherical ).status == Status::InvalidInput );
}

QTEST_MAIN( GeostatVariogramTests )
#include "tst_geostat_variogram.moc"
