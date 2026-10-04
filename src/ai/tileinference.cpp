// 层：功能
#include "tileinference.h"

#include "onnxpredictionservice.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// ai/ — tile 推理核心实现。纯函数（plan/softmax/stitch）先于执行器：缝合
// 正确性（每格恰好写一次）与 softmax 数值域独立可测。

QVector<InferenceTile> planInferenceTiles( int gridRows, int gridCols,
                                           int tileRows, int tileCols, int halo )
{
  QVector<InferenceTile> tiles;
  if ( gridRows <= 0 || gridCols <= 0 || tileRows <= 0 || tileCols <= 0 || halo < 0 )
    return tiles;
  const int rowSteps = ( gridRows + tileRows - 1 ) / tileRows;
  const int colSteps = ( gridCols + tileCols - 1 ) / tileCols;
  tiles.reserve( rowSteps * colSteps );
  for ( int r = 0; r < gridRows; r += tileRows )
  {
    const int innerR0 = r;
    const int innerR1 = qMin( r + tileRows, gridRows );
    const int readR0 = qMax( 0, innerR0 - halo );
    const int readR1 = qMin( gridRows, innerR1 + halo );
    for ( int c = 0; c < gridCols; c += tileCols )
    {
      const int innerC0 = c;
      const int innerC1 = qMin( c + tileCols, gridCols );
      const int readC0 = qMax( 0, innerC0 - halo );
      const int readC1 = qMin( gridCols, innerC1 + halo );
      InferenceTile t;
      t.row0 = readR0;
      t.col0 = readC0;
      t.rows = readR1 - readR0;
      t.cols = readC1 - readC0;
      t.innerRow0 = innerR0;
      t.innerCol0 = innerC0;
      t.innerRows = innerR1 - innerR0;
      t.innerCols = innerC1 - innerC0;
      tiles.append( t );
    }
  }
  return tiles;
}

int sanitizeModelInput( QVector<float> &data, QVector<bool> *valid )
{
  if ( valid )
    *valid = QVector<bool>( data.size(), true );
  int bad = 0;
  for ( qsizetype k = 0; k < data.size(); ++k )
  {
    if ( !std::isfinite( data[k] ) )
    {
      data[k] = 0.0f;
      if ( valid )
        ( *valid )[k] = false;
      ++bad;
    }
  }
  return bad;
}

void softmaxGrid( const QVector<float> &logits, int classes, int rows, int cols,
                  const QVector<bool> &valid, TileClassGrid *out )
{
  const int pixels = rows * cols;
  out->rows = rows;
  out->cols = cols;
  out->classes = classes;
  out->argmax = QVector<quint8>( pixels, 255 );
  out->confidence = QVector<float>( pixels, 0.0f );
  out->probMax = QVector<float>( pixels, 0.0f );
  if ( classes < 1 || classes > kMaxTileClasses || pixels < 1 ||
       logits.size() != qsizetype( classes ) * pixels )
    return; // 尺寸不符/类数越界 → 全 255（无数据）；调用方按契约先验形状
  const double lnC = classes > 1 ? std::log( double( classes ) ) : 1.0;
  for ( int p = 0; p < pixels; ++p )
  {
    if ( valid.size() == pixels && !valid[p] )
      continue; // 无数据：argmax 保持 255
    double cmax = -std::numeric_limits<double>::infinity();
    bool finite = true;
    for ( int c = 0; c < classes; ++c )
    {
      const double v = double( logits[c * pixels + p] );
      if ( !std::isfinite( v ) )
      {
        finite = false;
        break;
      }
      cmax = std::max( cmax, v );
    }
    if ( !finite )
      continue; // 非有限 logit：如实无数据（255/置信 0），不默认类 0
    double sum = 0.0;
    std::vector<double> expv( classes > 0 ? classes : 1, 0.0 );
    for ( int c = 0; c < classes; ++c )
    {
      expv[c] = std::exp( double( logits[c * pixels + p] ) - cmax );
      sum += expv[c];
    }
    double entropy = 0.0;
    int best = 0;
    double bestP = 0.0;
    for ( int c = 0; c < classes; ++c )
    {
      const double prob = expv[c] / sum;
      if ( prob > 0.0 )
        entropy -= prob * std::log( prob );
      if ( prob > bestP )
      {
        bestP = prob;
        best = c;
      }
    }
    out->argmax[p] = quint8( best );
    out->probMax[p] = float( bestP );
    double conf = classes > 1 ? 1.0 - entropy / lnC : 1.0;
    conf = std::clamp( conf, 0.0, 1.0 );
    out->confidence[p] = float( conf );
  }
}

void stitchTileIntoGrid( const InferenceTile &tile, const TileClassGrid &tileGrid,
                         TileClassGrid *grid )
{
  if ( !tile.isValid() || grid->argmax.isEmpty() || tileGrid.argmax.isEmpty() )
    return;
  const int offR = tile.innerRow0 - tile.row0;
  const int offC = tile.innerCol0 - tile.col0;
  for ( int ir = 0; ir < tile.innerRows; ++ir )
  {
    const int gridRow = tile.innerRow0 + ir;
    const int tileRow = offR + ir;
    for ( int ic = 0; ic < tile.innerCols; ++ic )
    {
      const int gridIdx = gridRow * grid->cols + tile.innerCol0 + ic;
      const int tileIdx = tileRow * tileGrid.cols + offC + ic;
      grid->argmax[gridIdx] = tileGrid.argmax[tileIdx];
      grid->confidence[gridIdx] = tileGrid.confidence[tileIdx];
      grid->probMax[gridIdx] = tileGrid.probMax[tileIdx];
    }
  }
}

bool runTileInference( PaleoOnnxService *onnx, const TileInferenceRequest &req,
                       TileInferenceResult *result,
                       const std::function<void( int, int )> &onProgress,
                       const std::function<bool()> &cancelRequested, QString *error )
{
  const auto fail = [error]( const QString &msg ) {
    if ( error )
      *error = msg;
    return false;
  };
  if ( error )
    error->clear();
  if ( !result )
    return fail( QStringLiteral( "no result sink" ) );
  if ( !onnx )
    return fail( QObject::tr( "tile inference needs an ONNX service" ) );
  if ( req.gridRows <= 0 || req.gridCols <= 0 )
    return fail( QObject::tr( "invalid output grid %1×%2" ).arg( req.gridRows ).arg( req.gridCols ) );
  if ( req.tileRows <= 0 || req.tileCols <= 0 || req.halo < 0 )
    return fail( QObject::tr( "invalid tile geometry %1×%2 halo %3" )
                   .arg( req.tileRows )
                   .arg( req.tileCols )
                   .arg( req.halo ) );
  if ( !req.fetch )
    return fail( QObject::tr( "tile inference needs a data source (fetch)" ) );

  const QVector<InferenceTile> tiles = planInferenceTiles(
    req.gridRows, req.gridCols, req.tileRows, req.tileCols, req.halo );
  if ( tiles.isEmpty() )
    return fail( QObject::tr( "tile planning produced no tiles" ) );
  result->tilesTotal = tiles.size();
  result->tilesDone = 0;
  result->inferenceMs = 0;
  result->elapsedMs = 0;

  // #144：加载与元数据一次取全（不读「最近一次加载」的全局 meta），推理绑定模型名。
  OnnxModelMeta meta;
  if ( onnx->loadModelMeta( req.model, &meta, error ) != OnnxLoadStatus::Ok )
    return fail( error && !error->isEmpty()
                   ? *error
                   : QObject::tr( "cannot load ONNX model '%1'" ).arg( req.model ) );

  // 输入签名门：4 维、N=1（或动态）、C=1（或动态）——tile 读域按 H×W 喂入。
  if ( meta.inputShape.size() != 4 ||
       ( meta.inputShape[0] > 1 ) || ( meta.inputShape[1] > 1 ) )
    return fail( QObject::tr( "模型输入须为 [1,1,H,W] 单通道 4 维，实际签名 %1" )
                   .arg( meta.inputSignature ) );

  TileClassGrid grid;
  grid.rows = req.gridRows;
  grid.cols = req.gridCols;
  grid.classes = 0;

  QElapsedTimer wall;
  wall.start();
  for ( int i = 0; i < tiles.size(); ++i )
  {
    const InferenceTile &tile = tiles[i];
    if ( cancelRequested && cancelRequested() )
    {
      result->elapsedMs = wall.elapsed();
      return fail( QObject::tr( "已取消（完成 %1/%2 tile）" ).arg( i ).arg( tiles.size() ) );
    }

    const qsizetype expected = qsizetype( tile.rows ) * tile.cols;
    QVector<float> data;
    QString fetchErr;
    if ( !req.fetch( tile.row0, tile.col0, tile.rows, tile.cols, data, fetchErr ) )
      return fail( QObject::tr( "tile %1/%2 取数失败: %3" ).arg( i + 1 ).arg( tiles.size() ).arg( fetchErr ) );
    if ( data.size() != expected )
      return fail( QObject::tr( "tile %1/%2 取数尺寸不符：期望 %3 实得 %4" )
                     .arg( i + 1 )
                     .arg( tiles.size() )
                     .arg( expected )
                     .arg( data.size() ) );

    // 固定输入维的模型：读域必须恰好匹配（动态维不受限）。
    if ( meta.inputShape[2] > 0 && meta.inputShape[2] != tile.rows )
      return fail( QObject::tr( "tile 读域 %1 行与模型固定输入 %2 行不符（换动态维模型或调 tile 尺寸）" )
                     .arg( tile.rows )
                     .arg( meta.inputShape[2] ) );
    if ( meta.inputShape[3] > 0 && meta.inputShape[3] != tile.cols )
      return fail( QObject::tr( "tile 读域 %1 列与模型固定输入 %2 列不符（换动态维模型或调 tile 尺寸）" )
                     .arg( tile.cols )
                     .arg( meta.inputShape[3] ) );

    QVector<bool> valid;
    sanitizeModelInput( data, &valid ); // #143：NaN/Inf 不进 ORT，输出端按掩膜置 255

    QString runErr;
    QElapsedTimer inferClock;
    inferClock.start();
    const OnnxTensor out =
      onnx->runTensorOn( req.model, meta.inputName, data, { 1, 1, tile.rows, tile.cols }, &runErr );
    const qint64 tileMs = inferClock.elapsed();
    result->inferenceMs += tileMs;
    if ( !runErr.isEmpty() )
      return fail( QObject::tr( "tile %1/%2 推理失败: %3" ).arg( i + 1 ).arg( tiles.size() ).arg( runErr ) );

    // 输出契约：[1,C,rows,cols]，空间维与读域一致。
    if ( out.shape.size() != 4 || out.shape[0] != 1 || out.shape[2] != tile.rows ||
         out.shape[3] != tile.cols || out.shape[1] < 1 ||
         out.values.size() != qsizetype( out.shape[1] ) * expected )
      return fail( QObject::tr( "tile %1/%2 输出形状 %3 与读域 %4×%5 不符（期望 [1,C,%4,%5]）" )
                     .arg( i + 1 )
                     .arg( tiles.size() )
                     .arg( [ &out ]() {
                       QString s;
                       for ( int64_t d : out.shape )
                         s += QString::number( d ) + QLatin1Char( ',' );
                       return s.isEmpty() ? s : s.left( s.size() - 1 );
                     }() )
                     .arg( tile.rows )
                     .arg( tile.cols ) );

    const int classes = int( out.shape[1] );
    if ( classes > kMaxTileClasses )
      return fail( QObject::tr( "模型输出 %1 类超过上限 %2（类号 255 保留为无数据）" )
                     .arg( classes )
                     .arg( kMaxTileClasses ) );
    if ( grid.classes == 0 )
    {
      grid.classes = classes;
      grid.argmax = QVector<quint8>( qsizetype( req.gridRows ) * req.gridCols, 255 );
      grid.confidence = QVector<float>( qsizetype( req.gridRows ) * req.gridCols, 0.0f );
      grid.probMax = QVector<float>( qsizetype( req.gridRows ) * req.gridCols, 0.0f );
    }
    else if ( grid.classes != classes )
    {
      return fail( QObject::tr( "tile %1 类数 %2 与首 tile 的 %3 不一致" )
                     .arg( i + 1 )
                     .arg( classes )
                     .arg( grid.classes ) );
    }

    TileClassGrid tileGrid;
    softmaxGrid( out.values, classes, tile.rows, tile.cols, valid, &tileGrid );
    stitchTileIntoGrid( tile, tileGrid, &grid );

    result->tilesDone = i + 1;
    if ( onProgress )
      onProgress( result->tilesDone, result->tilesTotal );
  }

  result->elapsedMs = wall.elapsed();
  result->grid = grid;
  return true;
}
