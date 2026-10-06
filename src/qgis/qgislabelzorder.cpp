// 层：QGIS 封装
#include "qgislabelzorder.h"

#include <qgsmaplayer.h>
#include <qgsmaprendererjob.h> // QGIS_PALEO_LABELS_WITH_LAYER（补丁版才定义）
#include <qgsproject.h>

#include <QtGlobal>

namespace
{
  // vendored QGIS 补丁识别的开关（见头文件注释；未打补丁时为空属性，无害）。
  const QString kFlagKey = QStringLiteral( "rendering/labelsWithLayer" );
  // 旧实现（renderAboveLabels 两带拆分）写过的属性——本类负责清残留。
  const QString kLegacyKey = QStringLiteral( "rendering/renderAboveLabels" );
}

QgisLabelZOrder::QgisLabelZOrder( QgsProject *project, QObject *parent )
  : QObject( parent )
  , m_project( project )
{
#ifndef QGIS_PALEO_LABELS_WITH_LAYER
  // #138 优雅降级留痕：apt/OSGeo4W 二进制路的 QGIS 没有 labelsWithLayer 补丁，
  // 属性写了也不生效，标注回到 QGIS 原生「永远置顶」。进程内只提示一次。
  static bool s_warned = false;
  if ( !s_warned )
  {
    s_warned = true;
    qInfo( "QgisLabelZOrder: QGIS 未打 labelsWithLayer 补丁，标注不随图层 z 序（原生置顶）" );
  }
#endif
  if ( !m_project )
    return;

  connect( m_project, &QgsProject::layersAdded, this,
           [this]( const QList<QgsMapLayer *> &layers ) {
             for ( QgsMapLayer *layer : layers )
               applyToLayer( layer );
           } );
  // readProject 会重建图层、且旧工程文件可能带回 renderAboveLabels
  // 残留属性——整表重打标一遍。
  connect( m_project, &QgsProject::readProject, this,
           [this] { refresh(); } );

  refresh();
}

bool QgisLabelZOrder::labelsWithLayerSupported()
{
#ifdef QGIS_PALEO_LABELS_WITH_LAYER
  return true;
#else
  return false;
#endif
}

void QgisLabelZOrder::applyToLayer( QgsMapLayer *layer )
{
  if ( !layer )
    return;

  bool changed = false;
  if ( !layer->customProperty( kFlagKey ).toBool() )
  {
    layer->setCustomProperty( kFlagKey, true );
    changed = true;
  }
  if ( layer->customProperty( kLegacyKey ).isValid() )
  {
    layer->removeCustomProperty( kLegacyKey );
    changed = true;
  }
  if ( changed )
    layer->triggerRepaint(); // 属性在 prepareJobs 时被读取，重绘后生效
}

void QgisLabelZOrder::refresh()
{
  if ( !m_project )
    return;

  const QMap<QString, QgsMapLayer *> layers = m_project->mapLayers();
  for ( QgsMapLayer *layer : layers )
    applyToLayer( layer );
}
