// 层：数据
// 地质统计留一交叉验证（P2 实现，P4 自 workflow 原样抽出）
#include "crossvalidation.h"

#include <algorithm>
#include <cmath>

namespace paleo::geostat
{

// P2：留一交叉验证（LOO）。逐井剔除（连同坐标重合的样本，避免重合样本让误差
// 虚低），用同一模型与邻域口径在该井位估值。样本超过上限时按固定步长抽检，
// 如实记录抽检数。predict 返回 ok=false 计入失败；方差 ≤ 0 的点不进标准化误差。
QVariantMap leaveOneOut( const std::vector<Sample> &samples, const QString &estimator, const CvPredictFn &predict,
                         const std::function<bool()> &cancelled, bool *wasCancelled )
{
  QVariantMap node;
  node.insert( QStringLiteral( "status" ), QStringLiteral( "computed" ) );
  node.insert( QStringLiteral( "estimator" ), estimator );
  node.insert( QStringLiteral( "scheme" ), QStringLiteral( "leave_one_out_drop_coincident" ) );
  const std::size_t n = samples.size();
  const std::size_t stride = n > kMaxCvSamples ? ( n + kMaxCvSamples - 1 ) / kMaxCvSamples : 1;
  double minX = 0, maxX = 0, minY = 0, maxY = 0;
  for ( std::size_t i = 0; i < n; ++i )
  {
    minX = i ? std::min( minX, samples[i].x ) : samples[i].x;
    maxX = i ? std::max( maxX, samples[i].x ) : samples[i].x;
    minY = i ? std::min( minY, samples[i].y ) : samples[i].y;
    maxY = i ? std::max( maxY, samples[i].y ) : samples[i].y;
  }
  const double eps = 1e-9 * std::max( 1.0, std::max( maxX - minX, maxY - minY ) );
  int attempted = 0, ok = 0, standardized = 0, coincidentDropped = 0;
  double sumErr = 0, sumSq = 0, sumAbs = 0, sumStdSq = 0;
  std::vector<Sample> rest;
  rest.reserve( n );
  for ( std::size_t i = 0; i < n; i += stride )
  {
    if ( cancelled && cancelled() )
    {
      *wasCancelled = true;
      return {};
    }
    rest.clear();
    for ( std::size_t j = 0; j < n; ++j )
    {
      if ( j == i )
        continue;
      if ( std::abs( samples[j].x - samples[i].x ) <= eps && std::abs( samples[j].y - samples[i].y ) <= eps )
      {
        ++coincidentDropped;
        continue;
      }
      rest.push_back( samples[j] );
    }
    ++attempted;
    if ( rest.empty() )
      continue;
    const CvPrediction prediction = predict( samples[i].x, samples[i].y, rest );
    if ( !prediction.ok || !std::isfinite( prediction.estimate ) )
      continue;
    ++ok;
    const double err = prediction.estimate - samples[i].value;
    sumErr += err;
    sumSq += err * err;
    sumAbs += std::abs( err );
    if ( std::isfinite( prediction.variance ) && prediction.variance > 1e-12 )
    {
      sumStdSq += err * err / prediction.variance;
      ++standardized;
    }
  }
  node.insert( QStringLiteral( "samples_total" ), static_cast<int>( n ) );
  node.insert( QStringLiteral( "stride" ), static_cast<int>( stride ) );
  node.insert( QStringLiteral( "attempted" ), attempted );
  node.insert( QStringLiteral( "estimated" ), ok );
  node.insert( QStringLiteral( "failed" ), attempted - ok );
  node.insert( QStringLiteral( "coincident_dropped" ), coincidentDropped );
  if ( ok > 0 )
  {
    node.insert( QStringLiteral( "mean_error" ), sumErr / ok );
    node.insert( QStringLiteral( "rmse" ), std::sqrt( sumSq / ok ) );
    node.insert( QStringLiteral( "mae" ), sumAbs / ok );
  }
  if ( standardized > 0 )
  {
    // 模型自洽时 ≈ 1：≫1 说明方差被低估（变程/基台/块金失配），≪1 说明高估。
    node.insert( QStringLiteral( "mean_standardized_squared_error" ), sumStdSq / standardized );
    node.insert( QStringLiteral( "standardized_count" ), standardized );
  }
  return node;
}

} // namespace paleo::geostat
