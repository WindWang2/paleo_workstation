// 层：功能
#include "modelregistry.h"

#include "onnxpredictionservice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace
{
// manifest shape 数组元素：整数；null/缺省 → -1（动态维）。
QVector<int64_t> parseShape( const QJsonArray &arr )
{
  QVector<int64_t> shape;
  for ( const QJsonValue &v : arr )
  {
    if ( v.isDouble() )
      shape.append( int64_t( v.toDouble() ) );
    else
      shape.append( -1 ); // null → 动态
  }
  return shape;
}

ModelRegistryEntry parseEntry( const QJsonObject &o )
{
  ModelRegistryEntry e;
  e.name = o.value( QStringLiteral( "name" ) ).toString();
  e.file = o.value( QStringLiteral( "file" ) ).toString();
  e.version = o.value( QStringLiteral( "version" ) ).toString();
  e.task = o.value( QStringLiteral( "task" ) ).toString();
  e.dataType = o.value( QStringLiteral( "dataType" ) ).toString();
  e.sha256Pinned = o.value( QStringLiteral( "sha256" ) ).toString();
  const QJsonObject input = o.value( QStringLiteral( "input" ) ).toObject();
  e.inputName = input.value( QStringLiteral( "name" ) ).toString();
  e.inputDtype = input.value( QStringLiteral( "dtype" ) ).toString();
  e.inputShape = parseShape( input.value( QStringLiteral( "shape" ) ).toArray() );
  const QJsonObject outputs = o.value( QStringLiteral( "outputs" ) ).toObject();
  e.outputSemantics = outputs.value( QStringLiteral( "semantics" ) ).toString();
  e.classes = outputs.value( QStringLiteral( "classes" ) ).toInt();
  return e;
}
} // namespace

bool ModelRegistryScan::anyRunnable() const
{
  for ( const ModelRegistryEntry &e : entries )
    if ( e.status == ModelRegistryEntry::Status::Ok )
      return true;
  return false;
}

QStringList ModelRegistryScan::runnableNames() const
{
  QStringList names;
  for ( const ModelRegistryEntry &e : entries )
    if ( e.status == ModelRegistryEntry::Status::Ok )
      names.append( e.name );
  return names;
}

ModelRegistryScan ModelRegistry::scan( const QString &modelsDir )
{
  ModelRegistryScan out;
  if ( modelsDir.isEmpty() || !QDir( modelsDir ).exists() )
    return out; // 目录都没有 = 未装模型（manifestFound=false，非错误）

  const QString manifestPath = QDir( modelsDir ).filePath( QStringLiteral( "manifest.json" ) );
  const QFileInfo info( manifestPath );
  if ( !info.exists() || !info.isFile() )
    return out; // 未装模型：如实降级，不是错误

  out.manifestFound = true;
  QFile f( manifestPath );
  if ( !f.open( QIODevice::ReadOnly ) )
  {
    out.manifestError = QObject::tr( "manifest.json 不可读: %1" ).arg( f.errorString() );
    return out;
  }
  QJsonParseError parseErr;
  const QJsonDocument doc = QJsonDocument::fromJson( f.readAll(), &parseErr );
  if ( parseErr.error != QJsonParseError::NoError || !doc.isObject() )
  {
    out.manifestError = parseErr.error != QJsonParseError::NoError
                          ? QObject::tr( "manifest.json 解析失败: %1" ).arg( parseErr.errorString() )
                          : QObject::tr( "manifest.json 根必须是对象" );
    return out;
  }
  const QJsonArray models = doc.object().value( QStringLiteral( "models" ) ).toArray();
  if ( models.isEmpty() )
  {
    // 空 models 数组：结构合法、没有模型——未装模型语义，不是错误。
    return out;
  }

  for ( const QJsonValue &v : models )
  {
    if ( !v.isObject() )
    {
      ModelRegistryEntry bad;
      bad.status = ModelRegistryEntry::Status::ManifestInvalid;
      bad.detail = QObject::tr( "models[] 元素必须是对象" );
      out.entries.append( bad );
      continue;
    }
    ModelRegistryEntry e = parseEntry( v.toObject() );
    if ( e.name.isEmpty() || e.file.isEmpty() )
    {
      e.status = ModelRegistryEntry::Status::ManifestInvalid;
      e.detail = QObject::tr( "条目缺 name/file 必填字段" );
      out.entries.append( e );
      continue;
    }
    e.absolutePath = QDir( modelsDir ).absoluteFilePath( e.file );
    if ( !QFileInfo::exists( e.absolutePath ) )
    {
      e.status = ModelRegistryEntry::Status::FileMissing;
      e.detail = QObject::tr( "模型文件缺失: %1" ).arg( e.absolutePath );
      out.entries.append( e );
      continue;
    }
    e.sha256Actual = PaleoOnnxService::sha256OfFile( e.absolutePath );
    if ( e.sha256Actual.isEmpty() )
    {
      e.status = ModelRegistryEntry::Status::Unreadable;
      e.detail = QObject::tr( "模型文件不可读: %1" ).arg( e.absolutePath );
      out.entries.append( e );
      continue;
    }
    if ( !e.sha256Pinned.isEmpty() && e.sha256Pinned.compare( e.sha256Actual, Qt::CaseInsensitive ) != 0 )
    {
      e.status = ModelRegistryEntry::Status::FingerprintMismatch;
      e.detail = QObject::tr( "钉哈希不符: manifest %1 vs 实测 %2（模型可能被替换）" )
                   .arg( e.sha256Pinned, e.sha256Actual );
      out.entries.append( e );
      continue;
    }
    out.entries.append( e );
  }
  return out;
}

QString ModelRegistry::statusLabel( ModelRegistryEntry::Status s )
{
  switch ( s )
  {
    case ModelRegistryEntry::Status::Ok: return QObject::tr( "可用" );
    case ModelRegistryEntry::Status::FileMissing: return QObject::tr( "文件缺失" );
    case ModelRegistryEntry::Status::ManifestInvalid: return QObject::tr( "manifest 条目不合法" );
    case ModelRegistryEntry::Status::FingerprintMismatch: return QObject::tr( "指纹不符" );
    case ModelRegistryEntry::Status::Unreadable: return QObject::tr( "不可读" );
  }
  return QString();
}
