// 层：功能
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

// NS1 里的 setError 调用的是 paleo::workflow_detail::setError（另一组跨 TU 内部
// 辅助，原先同 TU 可见）；独立成头后必须显式引入。
#include "workflows_internal.h"

// 方向20 轮4：constraintworkflow.cpp 二次拆分时的跨 TU 内部辅助。
// 原为该 .cpp 顶部的匿名命名空间（NS1，60-120 行）——实测这 5 个短工具被拆出的
// 两段**共用**（作业面侧 62 处），故进共享头；全部 inline 以免跨 TU 重复符号
// （教训来自 datapreviewtabs_internal.h 那次 LNK2005）。
// 函数体与拆分前逐字一致，仅去一级缩进。

namespace paleo::constraint_detail
{
/// 本段独占的内部辅助（无第二个消费者，故不进 workflows_internal.h）。
/// 与拆分前逐字一致，仅去一级缩进。
inline QString fileStem( const QString &path )
{
  const int slash = std::max( path.lastIndexOf( QLatin1Char( '/' ) ),
                              path.lastIndexOf( QLatin1Char( '\\' ) ) );
  const int dot = path.lastIndexOf( QLatin1Char( '.' ) );
  return dot > slash ? path.left( dot ) : path;
}

inline bool sameFile( const QString &left, const QString &right )
{
  if ( left.isEmpty() || right.isEmpty() )
    return false;
  return QDir::cleanPath( QFileInfo( left ).absoluteFilePath() ) ==
         QDir::cleanPath( QFileInfo( right ).absoluteFilePath() );
}

inline bool catalogPathMatches( const QString &projectDir, const QString &versionPath,
                         const QString &absolutePath )
{
  if ( versionPath.isEmpty() || absolutePath.isEmpty() )
    return false;
  const QDir base( projectDir );
  const QString stored = QDir::cleanPath( QDir::isAbsolutePath( versionPath )
                                             ? versionPath
                                             : base.absoluteFilePath( versionPath ) );
  const QString actual = QDir::cleanPath( QDir::isAbsolutePath( absolutePath )
                                             ? absolutePath
                                             : base.absoluteFilePath( absolutePath ) );
  return stored == actual;
}

inline void removeIfPresent( const QString &path )
{
  if ( !path.isEmpty() )
    QFile::remove( path );
}

inline QVariantMap readJsonObject( const QString &path, QString *error )
{
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) )
  {
    paleo::workflow_detail::setError( error, QStringLiteral( "无法读取 %1" ).arg( path ) );
    return {};
  }
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson( file.readAll(), &parseError );
  if ( parseError.error != QJsonParseError::NoError || !document.isObject() )
  {
    paleo::workflow_detail::setError( error, QStringLiteral( "不是 JSON 对象：%1" ).arg( path ) );
    return {};
  }
  return document.object().toVariantMap();
}


} // namespace paleo::constraint_detail
