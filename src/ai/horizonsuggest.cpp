// 层：功能
#include "horizonsuggest.h"

#include "onnxpredictionservice.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
struct Key
{
  int il, xl;
  bool operator==( const Key &o ) const { return il == o.il && xl == o.xl; }
};
uint qHash( const Key &k, uint seed = 0 )
{
  return ( uint( k.il ) * 31u + uint( k.xl ) ) ^ seed;
}

// Bernoulli 熵归一：p=峰概率；p→1 或 →0 → 置信 1；p=0.5 → 0。
float bernoulliConfidence( double p )
{
  p = std::clamp( p, 1e-12, 1.0 - 1e-12 );
  const double entropy = -( p * std::log( p ) + ( 1.0 - p ) * std::log( 1.0 - p ) );
  return float( std::clamp( 1.0 - entropy / std::log( 2.0 ), 0.0, 1.0 ) );
}
} // namespace

bool suggestHorizonTracking( PaleoOnnxService *onnx, const QString &model,
                             const QVector<TrackingSeed> &seeds, int windowSamples,
                             int radius, const TraceWindowFetcher &fetch,
                             QVector<TrackingSuggestion> *out, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return false;
  };
  if ( error )
    error->clear();
  if ( !out )
    return fail( QStringLiteral( "no output sink" ) );
  out->clear();
  if ( !onnx )
    return fail( QObject::tr( "追踪建议需要 ONNX 服务" ) );
  if ( seeds.isEmpty() )
    return fail( QObject::tr( "追踪建议需要至少一颗种子点" ) );
  if ( windowSamples < 2 || radius < 0 )
    return fail( QObject::tr( "非法参数：window=%1 radius=%2" ).arg( windowSamples ).arg( radius ) );
  if ( !fetch )
    return fail( QObject::tr( "追踪建议需要道窗取数回调" ) );

  if ( !onnx->isModelLoaded( model ) && !onnx->loadModel( model, error ) )
    return fail( error && !error->isEmpty() ? *error
                                            : QObject::tr( "无法加载模型 '%1'" ).arg( model ) );
  const OnnxModelMeta meta = onnx->loadedModelMeta();
  // scorer 契约：[1,1,T] 单通道 1D（NCL 布局）。
  if ( meta.inputShape.size() != 3 || meta.inputShape[0] > 1 || meta.inputShape[1] > 1 )
    return fail( QObject::tr( "trace scorer 模型输入须为 [1,1,T]，实际签名 %1" )
                   .arg( meta.inputSignature ) );

  QHash<Key, TrackingSuggestion> picked; // 已定拾取（含种子）
  QHash<Key, QPair<int, int>> anchor;    // 道 → 参考邻道
  const auto record = [&picked, &anchor]( const TrackingSuggestion &s ) {
    picked.insert( Key { s.inlineNo, s.xlineNo }, s );
  };

  for ( const TrackingSeed &seed : seeds )
  {
    TrackingSuggestion s;
    s.inlineNo = seed.inlineNo;
    s.xlineNo = seed.xlineNo;
    s.sampleIndex = seed.sampleIndex;
    s.score = 1.0f;
    s.confidence = 1.0f;
    s.isSeed = true;
    s.sourceInline = seed.inlineNo;
    s.sourceXline = seed.xlineNo;
    if ( !picked.contains( Key { seed.inlineNo, seed.xlineNo } ) )
      record( s );
  }

  // 扩张序：按（到最近种子的切比雪夫距离, inline, xline）——确定性。
  struct Candidate
  {
    int il, xl, dist;
  };
  QVector<Candidate> order;
  for ( const TrackingSeed &seed : seeds )
    for ( int dil = -radius; dil <= radius; ++dil )
      for ( int dxl = -radius; dxl <= radius; ++dxl )
      {
        const Key k { seed.inlineNo + dil, seed.xlineNo + dxl };
        if ( picked.contains( k ) )
          continue; // 种子或更早种子的邻域已覆盖
        const int dist = std::max( std::abs( dil ), std::abs( dxl ) );
        if ( std::find_if( order.cbegin(), order.cend(),
                           [ &k ]( const Candidate &c ) { return c.il == k.il && c.xl == k.xl; } ) ==
             order.cend() )
          order.append( Candidate { k.il, k.xl, dist } );
      }
  std::sort( order.begin(), order.end(), []( const Candidate &a, const Candidate &b ) {
    if ( a.dist != b.dist )
      return a.dist < b.dist;
    if ( a.il != b.il )
      return a.il < b.il;
    return a.xl < b.xl;
  } );

  for ( const Candidate &cand : order )
  {
    // 最近已拾取邻道（欧氏距离，平手取小 inline/xline——确定性）。
    const Key self { cand.il, cand.xl };
    const TrackingSuggestion *best = nullptr;
    int bestD2 = std::numeric_limits<int>::max();
    for ( auto it = picked.constBegin(); it != picked.constEnd(); ++it )
    {
      const int dil = it.key().il - cand.il;
      const int dxl = it.key().xl - cand.xl;
      const int d2 = dil * dil + dxl * dxl;
      if ( d2 < bestD2 || ( d2 == bestD2 && best &&
                            ( it.key().il < best->inlineNo ||
                              ( it.key().il == best->inlineNo && it.key().xl < best->xlineNo ) ) ) )
      {
        bestD2 = d2;
        best = &it.value();
      }
    }
    if ( !best )
      continue; // 理论不可达（种子先入 picked）

    const int center = best->sampleIndex;
    const int begin = center - windowSamples / 2;
    QVector<float> window;
    QString fetchErr;
    if ( !fetch( cand.il, cand.xl, begin, windowSamples, window, fetchErr ) )
      return fail( QObject::tr( "道 (%1,%2) 取数失败: %3" ).arg( cand.il ).arg( cand.xl ).arg( fetchErr ) );
    if ( window.size() != windowSamples )
      return fail( QObject::tr( "道 (%1,%2) 取窗尺寸不符：期望 %3 实得 %4" )
                     .arg( cand.il )
                     .arg( cand.xl )
                     .arg( windowSamples )
                     .arg( window.size() ) );

    int validCount = 0;
    for ( float v : window )
      if ( !std::isnan( v ) )
        ++validCount;
    if ( validCount < windowSamples / 2 )
      continue; // 死道：诚实留空（无建议），扩张链继续

    QString runErr;
    const OnnxTensor prob = onnx->runTensor( meta.inputName, window,
                                             { 1, 1, windowSamples }, &runErr );
    if ( !runErr.isEmpty() )
      return fail( QObject::tr( "道 (%1,%2) 打分失败: %3" ).arg( cand.il ).arg( cand.xl ).arg( runErr ) );
    if ( prob.shape.size() != 3 || prob.shape[0] != 1 || prob.shape[1] != 1 ||
         prob.shape[2] != windowSamples || prob.values.size() != windowSamples )
      return fail( QObject::tr( "道 (%1,%2) 打分输出形状 %3×%4 与窗长 %5 不符" )
                     .arg( cand.il )
                     .arg( cand.xl )
                     .arg( prob.shape.size() )
                     .arg( prob.values.size() )
                     .arg( windowSamples ) );

    int bestIdx = -1;
    float bestProb = -1.0f;
    for ( int i = 0; i < windowSamples; ++i )
    {
      if ( std::isnan( window[i] ) )
        continue; // 缺失样本不参与拾取
      const float p = prob.values[i];
      if ( !std::isfinite( p ) )
        continue;
      if ( p > bestProb )
      {
        bestProb = p;
        bestIdx = i;
      }
    }
    if ( bestIdx < 0 )
      continue; // 全无效窗：留空

    TrackingSuggestion s;
    s.inlineNo = cand.il;
    s.xlineNo = cand.xl;
    s.sampleIndex = begin + bestIdx;
    s.score = bestProb;
    s.confidence = bernoulliConfidence( double( bestProb ) );
    s.isSeed = false;
    s.sourceInline = best->inlineNo;
    s.sourceXline = best->xlineNo;
    record( s );
  }

  out->reserve( picked.size() );
  for ( auto it = picked.constBegin(); it != picked.constEnd(); ++it )
    out->append( it.value() );
  // 稳定输出序（QHash 迭代无序）：按 (inline, xline) 排序。
  std::sort( out->begin(), out->end(), []( const TrackingSuggestion &a, const TrackingSuggestion &b ) {
    if ( a.inlineNo != b.inlineNo )
      return a.inlineNo < b.inlineNo;
    return a.xlineNo < b.xlineNo;
  } );
  return true;
}
