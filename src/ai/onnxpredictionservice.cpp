// 层：功能
#include "onnxpredictionservice.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>

#include <cstring>
#include <vector>

#include <onnxruntime_cxx_api.h>

// ai/ — ET4 spike (ort_check) proved the vendored linux-x64 runtime runs a toy
// graph (y = x + 40.0) in-process and deterministic. This service keeps the
// header ORT-free: pool entries hold an opaque Ort::Session* so consumers only
// need Qt headers; the vendored include dir is required here alone.
//
// 会话管理（goal/ai-geological-assist）：
//   · 池：同一路径 + 同一 SHA256 的二次加载瞬时命中（不重建会话）；文件
//     指纹变化 → 旧会话淘汰重建（钉指纹语义）。LRU 上限 4，永不淘汰活跃项。
//   · warmup：首次建会话后按输入签名跑一次零填充推理并计时（meta.warmupMs）。
//   · 失败分类：OnnxLoadStatus；ORT 原始信息拼进错误链，不吞。
//   · 线程安全：m_mutex 盖住会话换装/池/Run；信号在锁外发（直连槽可安全回
//     调本服务）。

namespace
{
Ort::Env &ortEnv()
{
  // Leaked on purpose: Ort::Env must outlive every Ort::Session, and a
  // function-local static would be torn down at process exit while a session
  // could still be alive in a service instance — a classic shutdown race.
  static Ort::Env *env = new Ort::Env( ORT_LOGGING_LEVEL_WARNING, "paleo" );
  return *env;
}

QString libDirOf( const QString &root )
{
  // Canonical vendored layout is <root>/lib; the extracted release archive
  // keeps that shape, but be tolerant of a wrapped onnxruntime-<platform>-*/
  // directory one level down.
  const QString direct = root + QStringLiteral( "/lib" );
  if ( QDir( direct ).exists() )
    return direct;
#ifdef Q_OS_WIN
  QDirIterator it( root, { QStringLiteral( "onnxruntime*.dll" ) }, QDir::Files,
                   QDirIterator::Subdirectories );
#else
  QDirIterator it( root, { QStringLiteral( "libonnxruntime.so*" ) }, QDir::Files,
                   QDirIterator::Subdirectories );
#endif
  return it.hasNext() ? QFileInfo( it.next() ).absolutePath() : QString();
}

// ORT 异常信息 → 失败分类（保守归类；原始串永远进错误链）。
OnnxLoadStatus classifyLoadError( const QString &raw )
{
  const QString m = raw.toLower();
  if ( m.contains( QLatin1String( "ir version" ) ) ||
       m.contains( QLatin1String( "ir_version" ) ) )
    return OnnxLoadStatus::UnsupportedIr;
  if ( m.contains( QLatin1String( "protobuf" ) ) || m.contains( QLatin1String( "parse" ) ) ||
       m.contains( QLatin1String( "invalid onnx" ) ) || m.contains( QLatin1String( "onnx model" ) ) )
    return OnnxLoadStatus::BadFormat;
  return OnnxLoadStatus::CreateFailed;
}

QString loadStatusLabel( OnnxLoadStatus s )
{
  switch ( s )
  {
    case OnnxLoadStatus::NotFound: return QObject::tr( "模型文件不存在" );
    case OnnxLoadStatus::BadFormat: return QObject::tr( "模型格式不符（protobuf 解析失败）" );
    case OnnxLoadStatus::UnsupportedIr: return QObject::tr( "模型 IR 版本不兼容" );
    case OnnxLoadStatus::CreateFailed: return QObject::tr( "会话创建失败" );
    case OnnxLoadStatus::Ok: return QString();
  }
  return QString();
}

// 首输出推理（runTensor 与 warmup 共用；调用方持锁）。
bool runFirstOutput( Ort::Session *session, const QByteArray &inputNameUtf8,
                     const QVector<float> &input, const QVector<int64_t> &shape,
                     OnnxTensor *out, QString *error )
{
  try
  {
    Ort::AllocatorWithDefaultOptions allocator;
    if ( session->GetOutputCount() < 1 )
    {
      *error = QObject::tr( "Model has no outputs" );
      return false;
    }
    auto outputName = session->GetOutputNameAllocated( 0, allocator );

    Ort::MemoryInfo memoryInfo =
      Ort::MemoryInfo::CreateCpu( OrtArenaAllocator, OrtMemTypeDefault );
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
      memoryInfo, const_cast<float *>( input.constData() ),
      static_cast<size_t>( input.size() ), shape.constData(),
      static_cast<size_t>( shape.size() ) );

    const char *inputNames[] = { inputNameUtf8.constData() };
    const char *outputNames[] = { outputName.get() };
    auto outputs = session->Run( Ort::RunOptions { nullptr }, inputNames,
                                 &inputTensor, 1, outputNames, 1 );

    if ( outputs.empty() || !outputs[0].IsTensor() )
    {
      *error = QObject::tr( "Inference produced no tensor output" );
      return false;
    }
    auto info = outputs[0].GetTensorTypeAndShapeInfo();
    if ( info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT )
    {
      *error = QObject::tr( "Output tensor is not float32" );
      return false;
    }
    const size_t n = info.GetElementCount();
    const float *data = outputs[0].GetTensorData<float>();
    out->values.resize( static_cast<qsizetype>( n ) );
    std::memcpy( out->values.data(), data, n * sizeof( float ) );
    const std::vector<int64_t> dims = info.GetShape();
    out->shape.reserve( static_cast<qsizetype>( dims.size() ) );
    for ( int64_t d : dims )
      out->shape.append( d );
    return true;
  }
  catch ( const Ort::Exception &e )
  {
    QString msg = QString::fromUtf8( e.what() );
    const QString low = msg.toLower();
    if ( low.contains( QLatin1String( "invalid dimensions" ) ) ||
         low.contains( QLatin1String( "invalid rank" ) ) )
      msg.prepend( QObject::tr( "输入维度不符: " ) );
    *error = msg;
    return false;
  }
  catch ( const std::exception &e )
  {
    *error = QString::fromUtf8( e.what() );
    return false;
  }
}

QString elemTypeLabel( int t )
{
  if ( t == 1 ) return QStringLiteral( "float32" );
  if ( t == 7 ) return QStringLiteral( "int64" );
  return QStringLiteral( "type%1" ).arg( t );
}
} // namespace

struct PaleoOnnxService::PoolEntry
{
  Ort::Session *session = nullptr; // 池拥有；析构删
  OnnxModelMeta meta;
  quint64 lru = 0;
};

PaleoOnnxService::PaleoOnnxService( QObject *parent )
  : QObject( parent )
{
}

PaleoOnnxService::~PaleoOnnxService()
{
  QMutexLocker lock( &m_mutex );
  for ( PoolEntry *e : m_pool )
    delete e->session;
  qDeleteAll( m_pool );
  m_pool.clear();
  m_active = nullptr;
}

void PaleoOnnxService::setModelRoot( const QString &dir )
{
  QString nextRoot;
  if ( !dir.trimmed().isEmpty() )
  {
    const QFileInfo rootInfo( dir );
    const QString canonical = rootInfo.canonicalFilePath();
    nextRoot = QDir::cleanPath( canonical.isEmpty() ? rootInfo.absoluteFilePath() : canonical );
  }
  if ( nextRoot == m_modelRoot )
    return;

  // Sessions belong to one project's model directory. Drop the whole pool as
  // soon as that directory changes, even when the next project has a
  // same-named model.
  QMutexLocker lock( &m_mutex );
  for ( PoolEntry *e : m_pool )
    delete e->session;
  qDeleteAll( m_pool );
  m_pool.clear();
  m_active = nullptr;
  m_loaded.clear();
  m_loadedPath.clear();
  m_lastStatus = OnnxLoadStatus::NotFound;
  m_lastMeta = OnnxModelMeta();
  m_lastPoolHit = false;
  m_modelRoot = nextRoot;
}

QString PaleoOnnxService::normalizedModelPath( const QString &name ) const
{
  QString file = name;
  if ( !file.endsWith( QLatin1String( ".onnx" ) ) )
    file += QLatin1String( ".onnx" );
  const QFileInfo modelInfo( QDir( m_modelRoot ).absoluteFilePath( file ) );
  const QString canonical = modelInfo.canonicalFilePath();
  return QDir::cleanPath( canonical.isEmpty() ? modelInfo.absoluteFilePath() : canonical );
}

QStringList PaleoOnnxService::availableModels() const
{
  if ( m_modelRoot.isEmpty() )
    return {};
  const QDir dir( m_modelRoot );
  const QStringList files = dir.entryList( { QStringLiteral( "*.onnx" ) },
                                           QDir::Files, QDir::Name );
  QStringList names;
  names.reserve( files.size() );
  for ( const QString &f : files )
    names << QFileInfo( f ).completeBaseName(); // "toy.onnx" -> "toy" = loadModel arg
  return names;
}

QString PaleoOnnxService::sha256OfFile( const QString &path )
{
  QFile f( path );
  if ( !f.open( QIODevice::ReadOnly ) )
    return QString();
  QCryptographicHash hash( QCryptographicHash::Sha256 );
  constexpr qint64 kChunk = 1 << 20; // 1MiB：大模型不整读进内存
  QByteArray chunk( int( kChunk ), Qt::Uninitialized );
  while ( !f.atEnd() )
  {
    const qint64 n = f.read( chunk.data(), kChunk );
    if ( n <= 0 )
      return f.atEnd() ? QString::fromLatin1( hash.result().toHex() ) : QString();
    hash.addData( chunk.constData(), qsizetype( n ) );
  }
  return QString::fromLatin1( hash.result().toHex() );
}

bool PaleoOnnxService::loadModel( const QString &name, QString *error )
{
  return loadModelMeta( name, nullptr, error ) == OnnxLoadStatus::Ok;
}

OnnxLoadStatus PaleoOnnxService::loadModelMeta( const QString &name, OnnxModelMeta *meta,
                                                QString *error )
{
  if ( error )
    error->clear();
  QString file = name;
  if ( !file.endsWith( QLatin1String( ".onnx" ) ) )
    file += QLatin1String( ".onnx" );
  const QString path = normalizedModelPath( name );

  const auto finishFail = [this, error]( OnnxLoadStatus status, const QString &msg ) {
    QMutexLocker lock( &m_mutex );
    m_lastStatus = status;
    m_lastMeta = OnnxModelMeta();
    m_lastPoolHit = false;
    if ( error )
      *error = msg;
    return status;
  };

  if ( !QFileInfo::exists( path ) )
    return finishFail( OnnxLoadStatus::NotFound, tr( "Model not found: %1" ).arg( path ) );

  const QString sha = sha256OfFile( path );
  if ( sha.isEmpty() )
    return finishFail( OnnxLoadStatus::NotFound,
                       tr( "Model not readable: %1" ).arg( path ) );

  OnnxModelMeta nextMeta;
  QString emitName;

  {
    QMutexLocker lock( &m_mutex );
    // 锁内失败收尾（finishFail 不能再用——锁不可重入）。
    const auto failLocked = [this, error]( OnnxLoadStatus status, const QString &msg ) {
      m_lastStatus = status;
      m_lastMeta = OnnxModelMeta();
      m_lastPoolHit = false;
      if ( error )
        *error = msg;
      return status;
    };
    m_lastPoolHit = false;

    // 池命中：同路径 + 同指纹 → 直接换活跃指针（会话不重建、warmup 不重跑）。
    if ( PoolEntry *hit = m_pool.value( path ) )
    {
      if ( hit->meta.sha256 == sha )
      {
        m_active = hit;
        hit->lru = ++m_lruCounter;
        m_loaded = QFileInfo( file ).completeBaseName();
        m_loadedPath = path;
        m_lastStatus = OnnxLoadStatus::Ok;
        m_lastMeta = hit->meta;
        m_lastPoolHit = true;
        if ( meta )
          *meta = hit->meta;
        emitName = m_loaded; // 锁外发
      }
      else
      {
        // 文件指纹变了：钉指纹语义——旧会话淘汰，按新文件重建。
        m_pool.remove( path );
        if ( m_active == hit )
          m_active = nullptr;
        delete hit->session;
        delete hit;
      }
    }

    if ( emitName.isEmpty() )
    {
      Ort::Session *session = nullptr;
      try
      {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads( 1 );          // 池内会话统一钉 1 线程
        options.SetGraphOptimizationLevel( GraphOptimizationLevel::ORT_ENABLE_BASIC );
#ifdef _WIN32
        // Windows 的 Ort::Session 路径参数是 wchar_t（ORTCHAR_T）；toStdWString
        // 临时量存活到本语句结束，Session 构造期即拷贝路径，安全。
        const std::wstring pathW = path.toStdWString();
        session = new Ort::Session( ortEnv(), pathW.c_str(), options );
#else
        const QByteArray pathUtf8 = path.toUtf8();
        session = new Ort::Session( ortEnv(), pathUtf8.constData(), options );
#endif
      }
      catch ( const Ort::Exception &e )
      {
        const QString raw = QString::fromUtf8( e.what() );
        const OnnxLoadStatus status = classifyLoadError( raw );
        delete session;
        session = nullptr;
        return failLocked(
          status, tr( "%1: %2 (%3)" ).arg( loadStatusLabel( status ), path, raw ) );
      }
      catch ( const std::exception &e )
      {
        delete session;
        session = nullptr;
        return failLocked( OnnxLoadStatus::CreateFailed,
                           tr( "%1: %2 (%3)" )
                             .arg( loadStatusLabel( OnnxLoadStatus::CreateFailed ), path,
                                   QString::fromUtf8( e.what() ) ) );
      }

      // 输入签名（首输入；非 tensor 输入如实失败）。
      try
      {
        Ort::AllocatorWithDefaultOptions allocator;
        if ( session->GetInputCount() < 1 )
          throw std::runtime_error( "model has no inputs" );
        auto inputName = session->GetInputNameAllocated( 0, allocator );
        auto typeInfo = session->GetInputTypeInfo( 0 );
        auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
        nextMeta.inputElemType = int( tensorInfo.GetElementType() );
        const std::vector<int64_t> dims = tensorInfo.GetShape();
        QString sig = QStringLiteral( "%1:%2[" )
                        .arg( QString::fromUtf8( inputName.get() ),
                              elemTypeLabel( nextMeta.inputElemType ) );
        for ( size_t i = 0; i < dims.size(); ++i )
        {
          if ( i )
            sig += QLatin1Char( ',' );
          sig += dims[i] < 0 ? QStringLiteral( "?" ) : QString::number( dims[i] );
        }
        sig += QLatin1Char( ']' );
        nextMeta.inputName = QString::fromUtf8( inputName.get() );
        nextMeta.inputShape.reserve( qsizetype( dims.size() ) );
        for ( int64_t d : dims )
          nextMeta.inputShape.append( d );
        nextMeta.inputSignature = sig;
      }
      catch ( const std::exception &e )
      {
        delete session;
        return failLocked( OnnxLoadStatus::CreateFailed,
                           tr( "%1: 读取输入签名失败 (%2)" )
                             .arg( path, QString::fromUtf8( e.what() ) ) );
      }

      nextMeta.name = QFileInfo( file ).completeBaseName();
      nextMeta.path = path;
      nextMeta.sha256 = sha;

      // warmup：按输入签名零填充跑一次，计时（不伪造——失败如实记录）。
      qint64 elements = 1;
      bool allStatic = true;
      for ( int64_t d : nextMeta.inputShape )
      {
        if ( d <= 0 )
        {
          allStatic = false;
          break;
        }
        elements *= d;
      }
      const qint64 count = allStatic ? elements : 1; // 动态维 → 最小 1 元素冒烟
      if ( count >= 1 && count <= ( qint64( 4 ) << 20 ) )
      {
        OnnxTensor warm;
        const QVector<float> zeros( int( count ), 0.0f );
        QElapsedTimer clock;
        clock.start();
        const QByteArray inName = nextMeta.inputName.toUtf8();
        QString runErr;
        runFirstOutput( session, inName, zeros, nextMeta.inputShape, &warm, &runErr );
        nextMeta.warmupMs = clock.elapsed();
        nextMeta.warmupError = runErr;
      }
      else
      {
        nextMeta.warmupMs = -1;
        nextMeta.warmupError = tr( "跳过 warmup：静态输入过大（%1 元素）" ).arg( count );
      }

      auto *entry = new PoolEntry;
      entry->session = session;
      entry->meta = nextMeta;
      entry->lru = ++m_lruCounter;
      // LRU 容量 4：插不进就先淘汰最久未用（活跃项刚刷新 lru，不会被选中）。
      while ( m_pool.size() >= sessionPoolCapacity() )
      {
        PoolEntry *oldest = nullptr;
        for ( PoolEntry *e : m_pool )
          if ( !oldest || e->lru < oldest->lru )
            oldest = e;
        if ( !oldest )
          break;
        if ( m_active == oldest )
          m_active = nullptr; // 理论不可达（活跃项 lru 最新），防御性摘除
        m_pool.remove( oldest->meta.path );
        delete oldest->session;
        delete oldest;
      }
      m_pool.insert( path, entry );

      // All-or-nothing: only swap in after successful construction so a failed
      // load never clobbers a working session.
      m_active = entry;
      m_loaded = nextMeta.name;
      m_loadedPath = path;
      m_lastStatus = OnnxLoadStatus::Ok;
      m_lastMeta = nextMeta;
      m_lastPoolHit = false;
      if ( meta )
        *meta = nextMeta;
      emitName = m_loaded; // 锁外发
    }
  }

  if ( !emitName.isEmpty() )
    emit modelLoaded( emitName );
  return OnnxLoadStatus::Ok;
}

QString PaleoOnnxService::loadedModel() const
{
  QMutexLocker lock( &m_mutex );
  return m_loaded;
}

bool PaleoOnnxService::isModelLoaded( const QString &name ) const
{
  QMutexLocker lock( &m_mutex );
  if ( !m_active || m_loadedPath.isEmpty() )
    return false;
  const QString path = normalizedModelPath( name );
  return QFileInfo::exists( path ) && path == m_loadedPath;
}

OnnxLoadStatus PaleoOnnxService::lastLoadStatus() const
{
  QMutexLocker lock( &m_mutex );
  return m_lastStatus;
}

OnnxModelMeta PaleoOnnxService::loadedModelMeta() const
{
  QMutexLocker lock( &m_mutex );
  return m_lastMeta;
}

bool PaleoOnnxService::lastLoadPoolHit() const
{
  QMutexLocker lock( &m_mutex );
  return m_lastPoolHit;
}

int PaleoOnnxService::sessionPoolSize() const
{
  QMutexLocker lock( &m_mutex );
  return m_pool.size();
}

QVector<float> PaleoOnnxService::run( const QString &inputName, const QVector<float> &input,
                                      const QVector<int64_t> &shape, QString *error )
{
  return runTensor( inputName, input, shape, error ).values;
}

OnnxTensor PaleoOnnxService::runTensor( const QString &inputName, const QVector<float> &input,
                                        const QVector<int64_t> &shape, QString *error )
{
  if ( error )
    error->clear();
  QString failedModel;
  QString failMsg;
  OnnxTensor result;
  {
    QMutexLocker lock( &m_mutex );
    auto *session = m_active ? m_active->session : nullptr;
    if ( !session )
    {
      failedModel = m_loaded;
      failMsg = tr( "No model loaded" );
    }
    else
    {
      const QByteArray inNameUtf8 = inputName.toUtf8();
      if ( !runFirstOutput( session, inNameUtf8, input, shape, &result, &failMsg ) )
        failedModel = m_loaded; // 失败时 failMsg 已由 runFirstOutput 写入
    }
  }
  if ( !failMsg.isEmpty() )
  {
    if ( error )
      *error = failMsg;
    emit inferenceFailed( failedModel, failMsg );
    return {};
  }
  return result;
}

QString PaleoOnnxService::vendorRuntimeDir()
{
  // Vendor tree lives at <repo>/vendor/onnxruntime with include/ and lib/
  // directly inside (extracted release tarball, version 1.30.0). Walk up
  // from the binary so tests/selfcheck resolve it from any build dir.
  QDir dir( QCoreApplication::applicationDirPath() );
  for ( int i = 0; i < 10; ++i )
  {
    const QString candidate = dir.absoluteFilePath( QStringLiteral( "vendor/onnxruntime" ) );
    if ( QFileInfo::exists( candidate ) )
      return candidate;
    if ( !dir.cdUp() )
      break;
  }
  // Last resort: relative to the working directory, found or not — callers
  // get a meaningful path to report in errors.
  return QDir::current().absoluteFilePath( QStringLiteral( "vendor/onnxruntime" ) );
}

bool PaleoOnnxService::runtimeAvailable()
{
  const QString root = vendorRuntimeDir();
  if ( root.isEmpty() || !QDir( root ).exists() )
    return false;
  const QString libDir = libDirOf( root );
  if ( libDir.isEmpty() )
    return false;
#ifdef Q_OS_WIN
  // manifest.json onnxruntime_win.layout.bin：vendor/onnxruntime/lib/onnxruntime.dll
  return QFile::exists( libDir + QStringLiteral( "/onnxruntime.dll" ) );
#else
  // Accept the versioned soname or the unversioned dev symlink.
  return QFile::exists( libDir + QStringLiteral( "/libonnxruntime.so.1.30.0" ) ) ||
         QFile::exists( libDir + QStringLiteral( "/libonnxruntime.so" ) );
#endif
}
