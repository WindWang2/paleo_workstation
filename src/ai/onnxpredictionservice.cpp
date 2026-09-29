// 层：功能
#include "onnxpredictionservice.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

#include <cstring>
#include <vector>

#include <onnxruntime_cxx_api.h>

// ai/ — ET4 spike (ort_check) proved the vendored linux-x64 runtime runs a toy
// graph (y = x + 40.0) in-process and deterministic. This service keeps the
// header ORT-free: m_session is an opaque Ort::Session* so consumers only need
// Qt headers; the vendored include dir is required here alone.

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
} // namespace

PaleoOnnxService::PaleoOnnxService( QObject *parent )
  : QObject( parent )
{
}

PaleoOnnxService::~PaleoOnnxService()
{
  delete static_cast<Ort::Session *>( m_session );
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

  // A session belongs to one project's model directory. Drop it as soon as
  // that directory changes, even when the next project has a same-named model.
  delete static_cast<Ort::Session *>( m_session );
  m_session = nullptr;
  m_loaded.clear();
  m_loadedPath.clear();
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

bool PaleoOnnxService::loadModel( const QString &name, QString *error )
{
  if ( error )
    error->clear();
  QString file = name;
  if ( !file.endsWith( QLatin1String( ".onnx" ) ) )
    file += QLatin1String( ".onnx" );
  const QString path = normalizedModelPath( name );
  if ( !QFileInfo::exists( path ) )
  {
    if ( error )
      *error = tr( "Model not found: %1" ).arg( path );
    return false;
  }
  try
  {
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads( 1 );
    options.SetGraphOptimizationLevel( GraphOptimizationLevel::ORT_ENABLE_BASIC );
#ifdef _WIN32
    // Windows 的 Ort::Session 路径参数是 wchar_t（ORTCHAR_T）；toStdWString
    // 临时量存活到本语句结束，Session 构造期即拷贝路径，安全。
    const std::wstring pathW = path.toStdWString();
    auto *session = new Ort::Session( ortEnv(), pathW.c_str(), options );
#else
    const QByteArray pathUtf8 = path.toUtf8();
    auto *session = new Ort::Session( ortEnv(), pathUtf8.constData(), options );
#endif
    // All-or-nothing: only swap in after successful construction so a failed
    // load never clobbers a working session.
    delete static_cast<Ort::Session *>( m_session );
    m_session = session;
    m_loaded = QFileInfo( file ).completeBaseName();
    m_loadedPath = path;
    emit modelLoaded( m_loaded );
    return true;
  }
  catch ( const Ort::Exception &e )
  {
    if ( error )
      *error = QString::fromUtf8( e.what() );
  }
  catch ( const std::exception &e )
  {
    if ( error )
      *error = QString::fromUtf8( e.what() );
  }
  return false;
}

QString PaleoOnnxService::loadedModel() const
{
  return m_loaded;
}

bool PaleoOnnxService::isModelLoaded( const QString &name ) const
{
  if ( !m_session || m_loadedPath.isEmpty() )
    return false;
  const QString path = normalizedModelPath( name );
  return QFileInfo::exists( path ) && path == m_loadedPath;
}

QVector<float> PaleoOnnxService::run( const QString &inputName, const QVector<float> &input,
                                      const QVector<int64_t> &shape, QString *error )
{
  return runTensor( inputName, input, shape, error ).values;
}

OnnxTensor PaleoOnnxService::runTensor( const QString &inputName, const QVector<float> &input,
                                        const QVector<int64_t> &shape, QString *error )
{
  const auto fail = [this, error]( const QString &msg ) -> OnnxTensor {
    if ( error )
      *error = msg;
    emit inferenceFailed( m_loaded, msg );
    return {};
  };
  if ( error )
    error->clear();
  auto *session = static_cast<Ort::Session *>( m_session );
  if ( !session )
    return fail( tr( "No model loaded" ) );

  try
  {
    Ort::AllocatorWithDefaultOptions allocator;
    if ( session->GetOutputCount() < 1 )
      return fail( tr( "Model has no outputs" ) );
    auto outputName = session->GetOutputNameAllocated( 0, allocator );

    Ort::MemoryInfo memoryInfo =
      Ort::MemoryInfo::CreateCpu( OrtArenaAllocator, OrtMemTypeDefault );
    const QByteArray inNameUtf8 = inputName.toUtf8();
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
      memoryInfo, const_cast<float *>( input.constData() ),
      static_cast<size_t>( input.size() ), shape.constData(),
      static_cast<size_t>( shape.size() ) );

    const char *inputNames[] = { inNameUtf8.constData() };
    const char *outputNames[] = { outputName.get() };
    auto outputs = session->Run( Ort::RunOptions { nullptr }, inputNames,
                                 &inputTensor, 1, outputNames, 1 );

    if ( outputs.empty() || !outputs[0].IsTensor() )
      return fail( tr( "Inference produced no tensor output" ) );
    auto info = outputs[0].GetTensorTypeAndShapeInfo();
    if ( info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT )
      return fail( tr( "Output tensor is not float32" ) );
    const size_t n = info.GetElementCount();
    const float *data = outputs[0].GetTensorData<float>();
    OnnxTensor result;
    result.values.resize( static_cast<qsizetype>( n ) );
    std::memcpy( result.values.data(), data, n * sizeof( float ) );
    const std::vector<int64_t> dims = info.GetShape();
    result.shape.reserve( static_cast<qsizetype>( dims.size() ) );
    for ( int64_t d : dims )
      result.shape.append( d );
    return result;
  }
  catch ( const Ort::Exception &e )
  {
    return fail( QString::fromUtf8( e.what() ) );
  }
  catch ( const std::exception &e )
  {
    return fail( QString::fromUtf8( e.what() ) );
  }
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
