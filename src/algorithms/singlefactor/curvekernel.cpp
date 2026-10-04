// 层：数据
#include "curvekernel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

// 层：数据
namespace paleo::singlefactor
{
namespace
{

// numpy.polynomial.legendre.leggauss(48)，升序。核质量积分必须用同一组节点。
constexpr double kLegendreNodes[48] = {
    -9.98771007252426068e-01, -9.93530172266350764e-01, -9.84124583722826851e-01,
    -9.70591592546247273e-01, -9.52987703160430799e-01, -9.31386690706554332e-01,
    -9.05879136715569633e-01, -8.76572020274247854e-01, -8.43588261624393487e-01,
    -8.07066204029442624e-01, -7.67159032515740358e-01, -7.24034130923814634e-01,
    -6.77872379632663891e-01, -6.28867396776513599e-01, -5.77224726083972683e-01,
    -5.23160974722232996e-01, -4.66902904750958414e-01, -4.08686481990716721e-01,
    -3.48755886292160755e-01, -2.87362487355455554e-01, -2.24763790394689050e-01,
    -1.61222356068891709e-01, -9.70046992094626970e-02, -3.23801709628693674e-02,
    3.23801709628693674e-02,  9.70046992094626970e-02,  1.61222356068891709e-01,
    2.24763790394689050e-01,  2.87362487355455554e-01,  3.48755886292160755e-01,
    4.08686481990716721e-01,  4.66902904750958414e-01,  5.23160974722232996e-01,
    5.77224726083972683e-01,  6.28867396776513599e-01,  6.77872379632663891e-01,
    7.24034130923814634e-01,  7.67159032515740358e-01,  8.07066204029442624e-01,
    8.43588261624393487e-01,  8.76572020274247854e-01,  9.05879136715569633e-01,
    9.31386690706554332e-01,  9.52987703160430799e-01,  9.70591592546247273e-01,
    9.84124583722826851e-01,  9.93530172266350764e-01,  9.98771007252426068e-01 };
constexpr double kLegendreWeights[48] = {
    3.15334605230596253e-03, 7.32755390127620785e-03, 1.14772345792345902e-02,
    1.55793157229438557e-02, 1.96161604573555706e-02, 2.35707608393243423e-02,
    2.74265097083568818e-02, 3.11672278327981001e-02, 3.47772225647704492e-02,
    3.82413510658307226e-02, 4.15450829434647756e-02, 4.46745608566942939e-02,
    4.76166584924905475e-02, 5.03590355538544726e-02, 5.28901894851936602e-02,
    5.51995036999841718e-02, 5.72772921004031516e-02, 5.91148396983955660e-02,
    6.07044391658938601e-02, 6.20394231598926787e-02, 6.31141922862540478e-02,
    6.39242385846481714e-02, 6.44661644359501018e-02, 6.47376968126838620e-02,
    6.47376968126838620e-02, 6.44661644359501018e-02, 6.39242385846481714e-02,
    6.31141922862540478e-02, 6.20394231598926787e-02, 6.07044391658938601e-02,
    5.91148396983955660e-02, 5.72772921004031516e-02, 5.51995036999841718e-02,
    5.28901894851936602e-02, 5.03590355538544726e-02, 4.76166584924905475e-02,
    4.46745608566942939e-02, 4.15450829434647756e-02, 3.82413510658307226e-02,
    3.47772225647704492e-02, 3.11672278327981001e-02, 2.74265097083568818e-02,
    2.35707608393243423e-02, 1.96161604573555706e-02, 1.55793157229438557e-02,
    1.14772345792345902e-02, 7.32755390127620785e-03, 3.15334605230596253e-03 };

double coreMassFor( double radius, double core )
{
  const double c = std::clamp( core / radius, 0.0, 0.95 );
  const double limit = std::sqrt( std::max( 0.0, 1.0 - c * c ) );
  double sum = 0;
  for ( int i = 0; i < 48; ++i )
  {
    const double r = std::sqrt( ( kLegendreNodes[i] * limit ) * ( kLegendreNodes[i] * limit ) + c * c );
    sum += kLegendreWeights[i] * CurveKernel::basis( r );
  }
  return std::max( sum * limit * radius, 1e-20 );
}

} // namespace

double CurveKernel::basis( double r )
{
  const double t = std::max( 1.0 - r, 0.0 );
  return t * t * t * t * ( 4.0 * r + 1.0 );
}

CurveKernel::CurveKernel( const std::vector<Point2> &points, double radius, double core )
  : m_radius( radius )
{
  if ( !( m_radius > 0.0 ) || !std::isfinite( m_radius ) )
    m_radius = 1;
  const double usedCore = std::isfinite( core ) ? core : 0;
  m_coreMass = coreMassFor( m_radius, usedCore );
  if ( points.size() < 2 )
    return;
  for ( std::size_t i = 1; i < points.size(); ++i )
  {
    const double dx = points[i].x - points[i - 1].x;
    const double dy = points[i].y - points[i - 1].y;
    const double length = std::hypot( dx, dy );
    if ( length <= 1e-12 )
      continue;
    const int count = std::max( 1, static_cast<int>( std::ceil( length / ( m_radius * 0.15 ) ) ) );
    const double tx = dx / length;
    const double ty = dy / length;
    const std::array<double, 3> dyad{ tx * tx, tx * ty, ty * ty };
    for ( int k = 0; k < count; ++k )
    {
      const double t = ( static_cast<double>( k ) + 0.5 ) / static_cast<double>( count );
      m_centers.push_back( Point2{ points[i - 1].x + t * dx, points[i - 1].y + t * dy } );
      m_weights.push_back( length / static_cast<double>( count ) );
      m_dyads.push_back( dyad );
    }
  }
}

void CurveKernel::evaluateInto( const Point2 *points, int count, std::vector<double> &gate,
                                std::vector<std::array<double, 3>> &tensor ) const
{
  gate.assign( static_cast<std::size_t>( count ), 0 );
  tensor.assign( static_cast<std::size_t>( count ), std::array<double, 3>{ 0, 0, 0 } );
  if ( count <= 0 )
    return;
  std::vector<double> mass( static_cast<std::size_t>( count ), 0 );
  std::vector<std::array<double, 3>> moments( static_cast<std::size_t>( count ),
                                              std::array<double, 3>{ 0, 0, 0 } );
  const std::size_t centers = m_centers.size();
  for ( std::size_t start = 0; start < centers; start += 96 )
  {
    const std::size_t end = std::min( start + static_cast<std::size_t>( 96 ), centers );
    for ( int i = 0; i < count; ++i )
    {
      double rowMass = 0;
      double m0 = 0, m1 = 0, m2 = 0;
      for ( std::size_t k = start; k < end; ++k )
      {
        const double r = std::hypot( points[i].x - m_centers[k].x, points[i].y - m_centers[k].y ) / m_radius;
        const double w = basis( r ) * m_weights[k];
        rowMass += w;
        m0 += w * m_dyads[k][0];
        m1 += w * m_dyads[k][1];
        m2 += w * m_dyads[k][2];
      }
      mass[static_cast<std::size_t>( i )] += rowMass;
      auto &mom = moments[static_cast<std::size_t>( i )];
      mom[0] += m0;
      mom[1] += m1;
      mom[2] += m2;
    }
  }
  for ( int i = 0; i < count; ++i )
  {
    const double t = std::clamp( mass[static_cast<std::size_t>( i )] / m_coreMass, 0.0, 1.0 );
    gate[static_cast<std::size_t>( i )] = t * t * t * ( 10.0 - 15.0 * t + 6.0 * t * t );
    const double denom = std::max( mass[static_cast<std::size_t>( i )], 1e-30 );
    const auto &mom = moments[static_cast<std::size_t>( i )];
    tensor[static_cast<std::size_t>( i )] = { mom[0] / denom, mom[1] / denom, mom[2] / denom };
  }
}

CurveKernel::Eval CurveKernel::evaluate( const std::vector<Point2> &points ) const
{
  Eval out;
  evaluateInto( points.data(), static_cast<int>( points.size() ), out.gate, out.tensor );
  return out;
}

} // namespace paleo::singlefactor
