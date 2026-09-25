#include "mapversioncontroller.h"

#include "../metadata/layermanifest.h"
#include "../qgis/qgislayerservice.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <qgsmaplayer.h>
#include <qgsvectorlayer.h>

MapVersionController::MapVersionController( MapVersionStore *store, QgisLayerService *layers,
                                            QObject *parent )
  : QObject( parent )
  , m_store( store )
  , m_layers( layers )
{
}

MapVersion MapVersionController::saveVersion( const QString &horizon, const QVariantMap &provenance,
                                              QString *error )
{
  const QString json = QJsonDocument( QJsonObject::fromVariantMap( provenance ) )
                           .toJson( QJsonDocument::Compact );

  // 编辑会话 commit（§1223 版本提交边界）：该层位全部矢量图层逐一提交；
  // commitChanges 原生清 undo 栈，undoStack()->clear() 再兜底一次，保证
  // undo 不跨版本边界。栅格图层无编辑缓冲，跳过。
  if ( m_layers )
  {
    QVector<LayerDeclaration> declared;
    if ( !m_layers->tryDeclared( &declared, error ) )
      return MapVersion(); // 清单读失败时不得在未提交编辑的情况下出版本
    for ( const LayerDeclaration &d : declared )
    {
      if ( d.horizon != horizon )
        continue;
      if ( d.type.compare( QStringLiteral( "vector" ), Qt::CaseInsensitive ) != 0 )
        continue;
      QString instantiateErr;
      QgsMapLayer *layer = m_layers->instantiate( d.layerId, &instantiateErr );
      auto *vl = qobject_cast<QgsVectorLayer *>( layer );
      if ( !vl )
        continue; // 声明暂不可实例化（如源未落盘）不影响其他图层的提交
      if ( vl->isEditable() && !vl->commitChanges() )
      {
        if ( error )
          *error = tr( "图层 %1 提交编辑失败：%2" )
                       .arg( d.layerId, vl->commitErrors().join( QLatin1Char( ';' ) ) );
        return MapVersion();
      }
      vl->undoStack()->clear();
    }
  }

  MapVersion v = m_store ? m_store->saveVersion( horizon, json, error ) : MapVersion();
  if ( v.version > 0 )
    emit versionSaved( horizon, v.version );
  return v;
}

QString MapVersionController::publish( const QString &horizon, QString *error )
{
  if ( !m_store )
  {
    if ( error )
      *error = tr( "未绑定版本存储" );
    return QString();
  }
  QVector<LayerDeclaration> declared;
  if ( m_layers && !m_layers->tryDeclared( &declared, error ) )
    return QString(); // 清单读失败时不得发布空快照
  const QString dir = m_store->publish( horizon, declared, error );
  if ( !dir.isEmpty() )
    emit published( horizon, m_store->latest( horizon ).version, dir );
  return dir;
}
