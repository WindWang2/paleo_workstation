// 层：功能
#include "algorithmadapters.h"
#include "algorithmpublish.h"

namespace paleo
{

ProcessingAlgorithmAdapter::ProcessingAlgorithmAdapter( QgisProcessingService *proc )
  : m_proc( proc )
{
}

AlgorithmRunResult ProcessingAlgorithmAdapter::run(
    const AlgorithmSpec &spec, const QVariantMap &paramValues, QString *error,
    const QgisProcessingService::ProcessingHooks &hooks )
{
  AlgorithmRunResult r;
  if ( spec.id.isEmpty() )
  {
    r.ok = false;
    r.error = QStringLiteral( "ProcessingAlgorithmAdapter：AlgorithmSpec.id 为空" );
    if ( error )
      *error = r.error;
    return r;
  }
  if ( spec.engine != QStringLiteral( "processing" ) )
  {
    r.ok = false;
    r.error = QStringLiteral( "ProcessingAlgorithmAdapter：engine 不是 processing（%1）" )
                  .arg( spec.engine );
    if ( error )
      *error = r.error;
    return r;
  }
  if ( !m_proc )
  {
    r.ok = false;
    r.error = QStringLiteral( "ProcessingAlgorithmAdapter：未绑定 QgisProcessingService" );
    if ( error )
      *error = r.error;
    return r;
  }

  const QVariantMap results = m_proc->run( spec.id, paramValues, error, hooks );
  if ( results.isEmpty() )
  {
    r.ok = false;
    r.error = ( error && !error->isEmpty() )
                  ? *error
                  : QStringLiteral( "Processing 未返回结果（失败或取消）" );
    return r;
  }

  r = runResultFromProcessingOutput( results );
  if ( !r.ok )
  {
    r.error = QStringLiteral( "Processing 结果缺少 OUTPUT 路径" );
    if ( error )
      *error = r.error;
    return r;
  }

  // Spec 声明的 string 诊断端口 → diagnostics（不进 artifacts 路径）
  for ( const AlgoPort &p : spec.outputs )
  {
    if ( p.kind == QLatin1String( "string" ) && results.contains( p.id ) )
      r.diagnostics.insert( p.id, results.value( p.id ) );
  }
  // 保留其余结果键供 workflow 读 receipts 等
  for ( auto it = results.constBegin(); it != results.constEnd(); ++it )
  {
    if ( !r.diagnostics.contains( it.key() ) )
      r.diagnostics.insert( it.key(), it.value() );
  }
  return r;
}

AlgorithmRunResult ProcessingAlgorithmAdapter::runById(
    const QString &algorithmId, const QVariantMap &paramValues, QString *error,
    const QgisProcessingService::ProcessingHooks &hooks )
{
  const AlgorithmSpec spec = AlgorithmSpecRegistry::spec( algorithmId );
  if ( spec.id.isEmpty() )
  {
    // 未登记 Spec 时仍允许跑（兼容过渡）：用最小 Spec 壳
    AlgorithmSpec fallback;
    fallback.id = algorithmId;
    fallback.engine = QStringLiteral( "processing" );
    fallback.complete = false;
    fallback.outputs = { [] {
      AlgoPort o;
      o.id = QStringLiteral( "OUTPUT" );
      o.kind = QStringLiteral( "raster" );
      return o;
    }() };
    return run( fallback, paramValues, error, hooks );
  }
  return run( spec, paramValues, error, hooks );
}

AlgorithmRunResult NativeCppAdapter::run( const AlgorithmSpec &spec,
                                          const QVariantMap &paramValues,
                                          QString *error )
{
  Q_UNUSED( paramValues );
  AlgorithmRunResult r;
  r.ok = false;
  r.error = QStringLiteral( "NativeCppAdapter 尚未实现（algo-atom P2 桩）：%1" )
                .arg( spec.id );
  if ( error )
    *error = r.error;
  return r;
}

} // namespace paleo
