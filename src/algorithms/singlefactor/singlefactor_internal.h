// 层：数据
#pragma once

// 方向70（Unity 清障）：structuralalgorithm/localdirectionalgorithm 的
// sidecar JSON 落盘族（utf8/sidecarPath/writeJson/pointsJson/OutputGuard）
// 与 support/partition 的取消轮询 cancelled 原先各自匿名 namespace 复制一份
// 同构实现，UNITY_BUILD 合批即重定义。收拢单一定义；各 .cpp 经 `using`
// 接线，调用点零改动（先例 workflowerrors_internal.h）。

#include "types.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <qgsprocessingalgorithm.h>

#include <string>
#include <vector>

namespace paleo::singlefactor
{

/// 取消轮询：Control 带回调才视为取消。
inline bool cancelled( const Control *control )
{
  return control && control->cancelled && control->cancelled();
}

/// std::string → QString（UTF-8，显式长度防内嵌 NUL 截断）。
inline QString utf8( const std::string &text )
{
  return QString::fromUtf8( text.data(), static_cast<qsizetype>( text.size() ) );
}

/// 栅格 sidecar 落盘路径：<dir>/<basename><suffix>。
inline QString sidecarPath( const QString &rasterPath, const QString &suffix )
{
  const QFileInfo info( rasterPath );
  return info.absolutePath() + QLatin1Char( '/' ) + info.completeBaseName() + suffix;
}

/// JSON 落盘（缩进档）；打不开即抛 QgsProcessingException（算法失败语义）。
inline void writeJson( const QString &path, const QVariantMap &root )
{
  QFile file( path );
  if ( !file.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
    throw QgsProcessingException( QStringLiteral( "Cannot write %1" ).arg( path ) );
  file.write( QJsonDocument::fromVariant( root ).toJson( QJsonDocument::Indented ) );
}

/// 点列 → [[x,y],…]（sidecar JSON 几何分量）。
inline QVariantList pointsJson( const std::vector<Point2> &points )
{
  QVariantList list;
  for ( const Point2 &point : points )
  {
    QVariantList pair;
    pair << point.x << point.y;
    list << QVariant( pair );
  }
  return list;
}

/// 算法失败时清理未提交输出；keep=true 转正（析构即提交语义）。
struct OutputGuard
{
  QStringList paths;
  bool keep = false;
  ~OutputGuard()
  {
    if ( keep )
      return;
    for ( const QString &path : paths )
      QFile::remove( path );
  }
};

} // namespace paleo::singlefactor
