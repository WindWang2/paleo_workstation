// 层：功能
#include "onnxruntimeprobe.h"
#include "onnxpredictionservice.h"

#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <onnxruntime_c_api.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace paleo::onnx
{

namespace
{
// 测试注入存取受独立互斥保护：注入/还原与并发探测竞争时要么完整看到
// 注入函数、要么完全看不到，不产生半状态。
QMutex &overrideMutex()
{
  static QMutex m;
  return m;
}
std::function<OnnxRuntimeProbeResult()> &overrideFn()
{
  static std::function<OnnxRuntimeProbeResult()> fn;
  return fn;
}

void fillDetail( OnnxRuntimeProbeResult &r )
{
  switch ( r.status )
  {
    case OnnxRuntimeProbeResult::Status::Ok:
      r.detail = QObject::tr( "ONNX Runtime 就绪：API %1，运行库 %2（%3）" )
                     .arg( r.requiredApiVersion )
                     .arg( r.runtimeVersion.isEmpty() ? QObject::tr( "无法读取版本" ) : r.runtimeVersion )
                     .arg( r.loadedModulePath.isEmpty() ? QObject::tr( "加载位置未知" )
                                                        : r.loadedModulePath );
      break;
    case OnnxRuntimeProbeResult::Status::NotBuilt:
      r.detail = QObject::tr( "本构建未编入 ONNX Runtime（PALEO_HAVE_ORT=0），本地推理不可用" );
      break;
    case OnnxRuntimeProbeResult::Status::ApiUnavailable:
      r.detail = QObject::tr( "ONNX Runtime C API 不可用（OrtGetApiBase 返回空）。"
                              "进程内可观测的运行库缺失/加载不可用；所需 API %1。"
                              "实际加载模块：%2；受控前缀：%3。"
                              "若运行库文件缺失导致操作系统在进程启动前终止加载，"
                              "本探测无法覆盖——请按 BUILDING.md 重建受控前缀。" )
                     .arg( r.requiredApiVersion )
                     .arg( r.loadedModulePath.isEmpty() ? QObject::tr( "未知" ) : r.loadedModulePath )
                     .arg( r.controlledPrefix );
      break;
    case OnnxRuntimeProbeResult::Status::ApiVersionMismatch:
      r.detail = QObject::tr( "ONNX Runtime API 版本不匹配：编译所需 API %1，"
                              "实际运行库版本 %2。所需 API 在该运行库上返回空，"
                              "拒绝构造任何运行时对象（不降级所需 API、不回退系统库）。"
                              "实际加载模块：%3；受控前缀：%4。" )
                     .arg( r.requiredApiVersion )
                     .arg( r.runtimeVersion.isEmpty() ? QObject::tr( "无法读取" ) : r.runtimeVersion )
                     .arg( r.loadedModulePath.isEmpty() ? QObject::tr( "未知" ) : r.loadedModulePath )
                     .arg( r.controlledPrefix );
      break;
  }
}
} // namespace

QString OnnxRuntimeProbeResult::publicCode() const
{
  switch ( status )
  {
    case Status::Ok:
      return {};
    case Status::NotBuilt:
      return QStringLiteral( "runtime_not_built" );
    case Status::ApiUnavailable:
      return QStringLiteral( "runtime_api_unavailable" );
    case Status::ApiVersionMismatch:
      return QStringLiteral( "runtime_api_mismatch" );
  }
  return QStringLiteral( "runtime_api_unavailable" );
}

QString OnnxRuntimeProbeResult::summaryText() const { return detail; }

void OnnxRuntimeProbe::setTestOverride( const std::function<OnnxRuntimeProbeResult()> &fn )
{
  QMutexLocker lock( &overrideMutex() );
  overrideFn() = fn;
}

QString OnnxRuntimeProbe::loadedRuntimeModulePath()
{
#ifdef Q_OS_WIN
  HMODULE module = nullptr;
  if ( !GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT |
                                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>( &OrtGetApiBase ), &module ) ||
       !module )
    return {};
  wchar_t buffer[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW( module, buffer, MAX_PATH );
  return QString::fromWCharArray( buffer, int( n ) );
#else
  // /proc/self/maps 只读解析：libonnxruntime 的首个映射即加载模块路径。
  // 只报告内核观测事实；磁盘上存在候选库不等于该库已被加载。
  // 注意 /proc 文件 stat size 为 0——QTextStream/atEnd 会立即判真，必须
  // readAll 后按行拆。
  QFile maps( QStringLiteral( "/proc/self/maps" ) );
  if ( !maps.open( QIODevice::ReadOnly ) )
    return {};
  const QStringList rows = QString::fromLatin1( maps.readAll() ).split( QLatin1Char( '\n' ) );
  for ( const QString &row : rows )
  {
    const int pathStart = row.indexOf( QLatin1Char( '/' ) );
    if ( pathStart < 0 )
      continue;
    const QString path = row.mid( pathStart ).trimmed();
    if ( path.contains( QStringLiteral( "libonnxruntime" ), Qt::CaseInsensitive ) &&
         QFileInfo( path ).isFile() )
      return QFileInfo( path ).canonicalFilePath();
  }
  return {};
#endif
}

OnnxRuntimeProbeResult OnnxRuntimeProbe::probe()
{
  {
    QMutexLocker lock( &overrideMutex() );
    if ( overrideFn() )
    {
      // 注入只负责分类字段；诊断文案按同一 fillDetail 组装（测试注入不必
      // 重复拼接文案，结论与真实探测同构）。
      OnnxRuntimeProbeResult r = overrideFn()();
      if ( r.detail.isEmpty() )
      {
        if ( r.controlledPrefix.isEmpty() )
          r.controlledPrefix = PaleoOnnxService::vendorRuntimeDir();
        fillDetail( r );
      }
      return r;
    }
  }
  OnnxRuntimeProbeResult r;
  r.requiredApiVersion = ORT_API_VERSION;
  r.controlledPrefix = PaleoOnnxService::vendorRuntimeDir();
  r.loadedModulePath = loadedRuntimeModulePath();

  // 只调 C API，不构造任何 Ort:: 包装对象——OrtApiBase/OrtApi 是纯 C 结构，
  // GetApi 返回空时这里能如实判定，包装对象则会在构造内解引用空指针。
  const OrtApiBase *apiBase = OrtGetApiBase();
  if ( !apiBase )
  {
    r.status = OnnxRuntimeProbeResult::Status::ApiUnavailable;
    fillDetail( r );
    return r;
  }
  if ( apiBase->GetVersionString )
    r.runtimeVersion = QString::fromUtf8( apiBase->GetVersionString() );
  const OrtApi *api = apiBase->GetApi( ORT_API_VERSION );
  if ( !api )
  {
    r.status = OnnxRuntimeProbeResult::Status::ApiVersionMismatch;
    fillDetail( r );
    return r;
  }
  r.status = OnnxRuntimeProbeResult::Status::Ok;
  fillDetail( r );
  return r;
}

} // namespace paleo::onnx
