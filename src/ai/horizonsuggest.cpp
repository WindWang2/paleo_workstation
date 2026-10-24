// 层：功能
#include <cstdlib>
#include "horizonsuggest.h"

#include "onnxpredictionservice.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <queue>
#include <tuple>
#include <vector>

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

// 最近已拾取场（#479 去平方化）：格 → 最近 picked 点（欧氏距离平方 + 平手
// 取小 inline/xline 的字典序）。种子/拾取按处理序增量播撒（Dijkstra 式向
// 外传播，仅在「更近」时覆盖），查询 O(1)——替代逐候选全量扫 picked 的
// O(n·m)。域限制在候选包围盒内。
struct NearestField
{
  struct Rec
  {
    int d2 = 0;
    int il = 0, xl = 0; // 最近 picked 点坐标（平手裁决键）
  };
  QHash<Key, Rec> grid;
  int minIl = 0, maxIl = 0, minXl = 0, maxXl = 0;

  NearestField( int a, int b, int c, int d )
    : minIl( a ), maxIl( b ), minXl( c ), maxXl( d )
  {
  }
  bool inBounds( int il, int xl ) const
  {
    return il >= minIl && il <= maxIl && xl >= minXl && xl <= maxXl;
  }
  // 字典序：d2 → inline → xline（与旧全量扫描的平手规则一致）。
  static bool betterThan( const Rec &a, const Rec &b )
  {
    return std::tie( a.d2, a.il, a.xl ) < std::tie( b.d2, b.il, b.xl );
  }
  void seed( int il, int xl )
  {
    // 优先队列按 d2 升序弹出；仅当候选记录（距离平方 + 平手裁决键）严格
    // 更优才覆盖并继续传播——每格写入次数摊还后 O(1)，总代价 ~O(域)。
    using Item = std::tuple<int, int, int>; // (d2, 格il, 格xl)
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    pq.push( { 0, il, xl } );
    static const int kDIl[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
    static const int kDXl[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
    while ( !pq.empty() )
    {
      const auto [ d2, curIl, curXl ] = pq.top();
      pq.pop();
      const Key k { curIl, curXl };
      const Rec candidate { d2, il, xl };
      const auto it = grid.constFind( k );
      if ( it != grid.constEnd() && !betterThan( candidate, it.value() ) )
        continue; // 已有等优/更优记录：剪枝
      grid.insert( k, candidate );
      for ( int i = 0; i < 8; ++i )
      {
        const int nIl = curIl + kDIl[i];
        const int nXl = curXl + kDXl[i];
        if ( !inBounds( nIl, nXl ) )
          continue;
        const int dIl = nIl - il, dXl = nXl - xl;
        pq.push( { dIl * dIl + dXl * dXl, nIl, nXl } );
      }
    }
  }
};

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
                             const std::function<bool()> &cancelled,
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
  // PW-013：纵深防御——radius 与种子坐标设上限，避免 seed±radius 整型溢出与
  // O(seeds·(2r+1)²) 候选爆炸（调用方的工具层已先行拒绝）。
  constexpr int kMaxRadius = 4096;
  constexpr int kMaxCoordAbs = 100000000;
  if ( radius > kMaxRadius )
    return fail( QObject::tr( "非法参数：radius=%1 超过上限 %2" ).arg( radius ).arg( kMaxRadius ) );
  for ( const TrackingSeed &seed : seeds )
    if ( std::abs( seed.inlineNo ) > kMaxCoordAbs || std::abs( seed.xlineNo ) > kMaxCoordAbs )
      return fail( QObject::tr( "非法种子点坐标 (%1,%2)" ).arg( seed.inlineNo ).arg( seed.xlineNo ) );

  OnnxModelMeta meta; // #144：meta 随加载取回，推理绑定模型名
  if ( onnx->loadModelMeta( model, &meta, error ) != OnnxLoadStatus::Ok )
    return fail( error && !error->isEmpty() ? *error
                                            : QObject::tr( "无法加载模型 '%1'" ).arg( model ) );
  // scorer 契约：[1,1,T] 单通道 1D（NCL 布局）。
  if ( meta.inputShape.size() != 3 || meta.inputShape[0] > 1 || meta.inputShape[1] > 1 )
    return fail( QObject::tr( "trace scorer 模型输入须为 [1,1,T]，实际签名 %1" )
                   .arg( meta.inputSignature ) );

  QHash<Key, TrackingSuggestion> picked; // 已定拾取（含种子）
  // #479 去平方化：候选域包围盒（种子 ± radius）限定距离场传播范围。
  int minIl = seeds.first().inlineNo, maxIl = minIl;
  int minXl = seeds.first().xlineNo, maxXl = minXl;
  for ( const TrackingSeed &seed : seeds )
  {
    minIl = std::min( minIl, seed.inlineNo - radius );
    maxIl = std::max( maxIl, seed.inlineNo + radius );
    minXl = std::min( minXl, seed.xlineNo - radius );
    maxXl = std::max( maxXl, seed.xlineNo + radius );
  }
  NearestField nearest( minIl, maxIl, minXl, maxXl );
  const auto record = [&picked, &nearest]( const TrackingSuggestion &s ) {
    picked.insert( Key { s.inlineNo, s.xlineNo }, s );
    nearest.seed( s.inlineNo, s.xlineNo );
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
  // 去重 #479：QSet 一次遍历（原 std::find_if 线性查重是 O(n²)）。
  struct Candidate
  {
    int il, xl, dist;
  };
  QSet<Key> seen; // 种子/已入序候选（键去重）
  QVector<Candidate> order;
  for ( const TrackingSeed &seed : seeds )
    for ( int dil = -radius; dil <= radius; ++dil )
      for ( int dxl = -radius; dxl <= radius; ++dxl )
      {
        const Key k { seed.inlineNo + dil, seed.xlineNo + dxl };
        if ( picked.contains( k ) || seen.contains( k ) )
          continue; // 种子或更早种子/本种子的邻域已覆盖
        seen.insert( k );
        const int dist = std::max( std::abs( dil ), std::abs( dxl ) );
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
    // 协作取消：逐道扩张前检查；命中即如实失败，不把半途建议当成功外发。
    if ( cancelled && cancelled() )
      return fail( QObject::tr( "已取消" ) );
    // 最近已拾取邻道（#479：距离场 O(1) 查询替代全量扫 picked 的 O(m)；
    // 平手取小 inline/xline 的裁决与距离场 Rec 字典序一致）。
    const Key self { cand.il, cand.xl };
    const auto nearIt = nearest.grid.constFind( self );
    if ( nearIt == nearest.grid.constEnd() )
      continue; // 理论不可达（种子先入距离场）
    const auto pickedIt = picked.constFind( Key { nearIt->il, nearIt->xl } );
    if ( pickedIt == picked.constEnd() )
      continue; // 理论不可达（距离场源必在 picked）
    const TrackingSuggestion *best = &pickedIt.value();

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
      if ( std::isfinite( v ) )
        ++validCount;
    if ( validCount < windowSamples / 2 )
      continue; // 死道：诚实留空（无建议），扩张链继续

    QString runErr;
    const OnnxTensor prob = onnx->runTensorOn( model, meta.inputName, window,
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
      if ( !std::isfinite( window[i] ) )
        continue; // 缺失/非有限样本不参与拾取（±Inf 同 NaN 口径）
      const float p = prob.values[i];
      if ( !std::isfinite( p ) )
        continue;
      // 输出域守卫：trace scorer 契约是概率 [0,1]（fixture/注册表口径）。
      // logits/未归一输出会得 score>1、置信恒 1——静默钳位等于向解释员
      // 报送全置信建议。如实拒绝整个模型（宁可拒绝不建议）。
      if ( p < -1e-3f || p > 1.0f + 1e-3f )
        return fail( QObject::tr( "trace scorer「%1」输出 p=%2 越出概率域 [0,1]"
                                  "（疑似 logits/未归一输出）——拒绝建议" )
                       .arg( model )
                       .arg( double( p ), 0, 'g', 6 ) );
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
