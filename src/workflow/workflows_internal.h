// 层：功能
#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVariantMap>

class QgisLayerService;
class QObject;
class DerivedAssetRegistrar;

namespace paleo::workflow_detail {

/// 三段式各段统一的失败文案落点：*error 非空才写。
inline void setError( QString *error, const QString &text )
{
  if ( error )
    *error = text;
}

/// 结果图层 id ↔ 资产 id 的互链（写在 QgsMapLayer 的自定义属性上）。
/// QgisLayerService/QgsMapLayer 只在 .cpp 侧可见，故这里只前向声明。
void stampLayerAssetLink( QgisLayerService *layers, const QString &layerId,
                          const QString &assetId );

/// 派生产物登记通道（catalog 与 projectDir 走 QObject 动态属性，见 workflows.h）。
/// 返回值是按值的 registrar，需要完整类型，故定义在 workflows.cpp。
DerivedAssetRegistrar derivedRegistrarOf( const QObject *wf, QString *error );

/// OUTPUT 是约定的目的地键；为空时回落到首个字符串值的结果，让目的地命名
/// 不同的算法也能声明产物。
inline QString outputPathOf( const QVariantMap &results )
{
  QString out = results.value( QStringLiteral( "OUTPUT" ) ).toString();
  if ( out.isEmpty() )
  {
    for ( const QVariant &v : results )
    {
      if ( v.typeId() == QMetaType::QString && !v.toString().isEmpty() )
      {
        out = v.toString();
        break;
      }
    }
  }
  return out;
}

/// 已声明的源在「基底（去掉 "|layername=" 之类后缀）是本地文件系统路径」时算
/// file-backed。内存伪 URI 与远程/VSI 源无法用 QFile 检查，跳过。
inline bool isFileBackedSource( const QString &source )
{
  if ( source.isEmpty() )
    return false;
  const QString base = source.section( QLatin1Char( '|' ), 0, 0 );
  if ( base.isEmpty() )
    return false;
  if ( base.startsWith( QStringLiteral( "memory" ), Qt::CaseInsensitive ) )
    return false;
  if ( base.contains( QLatin1Char( '?' ) ) )          // memory provider URI ("Point?crs=...")
    return false;
  if ( base.contains( QStringLiteral( "://" ) ) )     // remote URI
    return false;
  if ( base.startsWith( QStringLiteral( "/vsi" ) ) )  // GDAL virtual filesystem
    return false;
  return true;
}

/// 紧凑 UTC 时间戳——让结果图层 id 逐次运行唯一。
inline QString stamp()
{
  return QDateTime::currentDateTimeUtc().toString( QStringLiteral( "yyyyMMdd-hhmmss-zzz" ) );
}

} // namespace paleo::workflow_detail

