// 层：QGIS 封装
#include "qgislabelzorder.h"

#include <qgsmaplayer.h>
#include <qgsproject.h>

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
