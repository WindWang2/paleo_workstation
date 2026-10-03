// 层：功能
#include "workflows.h"
#include "workflows_internal.h"

#include "../ai/onnxpredictionservice.h" // ORT-free header; symbol refs are PALEO_HAVE_ORT-guarded
#include "../catalog/datacatalog.h"      // localGridCrsWkt — ONNX 栅格落在局部测网
#include "../io/constraintstore.h"
#include "../domain/arearules.h"
#include "../domain/singlefactorrequest.h" // 制图工作场不进融合/分相
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgiseditingservice.h" // 拓扑提交门（geometryCommitError）
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
// ---- m2(B): 单因素生成链（ConstraintWorkflow::generateFactor/generateContours）----
#include "../qgis/factorcontour.h"
#include "../qgis/factorstylewriter.h"
#include "../qgis/qgisstyleservice.h" // C2：applyFaciesBoundaryStyle（相界语义符号）
#include "../algorithms/singlefactor/cartographicworkfile.h"
#include "../services/singlefactordef.h"
#include "../algorithms/geostat/kriging.h" // 方向18：克里金/SGS 纯数值核（数据层直连）
#include "../algorithms/geostat/sgs.h"
#include "../algorithms/geostat/variogram.h"
#include "../algorithms/rasterout.h" // 方向18：GeoTIFF 写出口
#include "boundarysemantics.h" // C2：相界地质语义类型词表
// ---- m2(B) end ----
#include "../services/jobrunner.h"
#include "../services/projectdata.h"
#include "derivedassets.h"
#include "mappingworkflow.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUuid>
#include <QVariantList>

#include <qgscoordinatereferencesystem.h>
#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsfield.h>
#include <qgsgeometry.h>
#include <qgsmaplayer.h>
#include <qgsrasterlayer.h>
#include <qgsvectorlayer.h> // ---- m2(C)：saveFaciesAttributes 的 edit buffer 回写 ------

#include <gdal.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

// ---------------------------------------------------------------------------
// workflows.h fixes the public shape of these classes and declares no data
// members. Service bindings and per-instance state therefore ride on dynamic
// QObject properties — QObject owns the storage, so nothing needs manual
// cleanup when a workflow is destroyed.
// ---------------------------------------------------------------------------

namespace
{
  const char kCatalogProp[]     = "paleo.wf.catalog";     // QObject* (DataCatalog) — T26 派生产物登记
  const char kProjectDirProp[]  = "paleo.wf.projectdir";  // QString — 受管 artifacts/ 根



} // namespace

void PaleoWorkflowBindDerivedCatalog( QObject *workflow, DataCatalog *catalog,
                                      const QString &projectDir )
{
  if ( !workflow )
    return;
  if ( auto *pw = qobject_cast<PredictionWorkflow *>( workflow ) )
    pw->setCatalog( catalog, projectDir );
  else if ( auto *cw = qobject_cast<ConstraintWorkflow *>( workflow ) )
    cw->setCatalog( catalog, projectDir );
  else if ( auto *comp = qobject_cast<CompositionWorkflow *>( workflow ) )
    comp->setCatalog( catalog, projectDir );
  else
  {
    workflow->setProperty( kCatalogProp, QVariant::fromValue( static_cast<QObject *>( catalog ) ) );
    workflow->setProperty( kProjectDirProp, projectDir );
  }
}

DataCatalog *PaleoWorkflowDerivedCatalog( const QObject *workflow )
{
  if ( !workflow )
    return nullptr;
  if ( auto *pw = qobject_cast<const PredictionWorkflow *>( workflow ) )
    return pw->catalog();
  if ( auto *cw = qobject_cast<const ConstraintWorkflow *>( workflow ) )
    return cw->catalog();
  if ( auto *comp = qobject_cast<const CompositionWorkflow *>( workflow ) )
    return comp->catalog();
  return qobject_cast<DataCatalog *>( workflow->property( kCatalogProp ).value<QObject *>() );
}

QString PaleoWorkflowDerivedProjectDir( const QObject *workflow )
{
  if ( !workflow )
    return QString();
  if ( auto *pw = qobject_cast<const PredictionWorkflow *>( workflow ) )
    return pw->projectDir();
  if ( auto *cw = qobject_cast<const ConstraintWorkflow *>( workflow ) )
    return cw->projectDir();
  if ( auto *comp = qobject_cast<const CompositionWorkflow *>( workflow ) )
    return comp->projectDir();
  return workflow->property( kProjectDirProp ).toString();
}

// ---- m2(C) end --------------------------------------------------------------

// ---------------------------------------------------------------------------
// 跨 TU 内部辅助的非 inline 部分（方向20 轮4）。
// 声明在 workflows_internal.h；这两个需要 QgisLayerService / DerivedAssetRegistrar
// 的完整类型，故定义留在本 TU。
// ---------------------------------------------------------------------------

namespace paleo::workflow_detail {

void stampLayerAssetLink( QgisLayerService *layers, const QString &layerId,
                          const QString &assetId )
{
  if ( !layers || layerId.isEmpty() || assetId.isEmpty() )
    return;
  if ( QgsMapLayer *l = layers->layer( layerId ) )
    l->setCustomProperty( QStringLiteral( "paleoAssetId" ), assetId );
}

DerivedAssetRegistrar derivedRegistrarOf( const QObject *wf, QString *error )
{
  DataCatalog *catalog = PaleoWorkflowDerivedCatalog( wf );
  if ( !catalog )
  {
    paleo::workflow_detail::setError( error, QObject::tr( "工作流未绑定数据目录（catalog）——派生产物无法登记到工程" ) );
    return DerivedAssetRegistrar();
  }
  return DerivedAssetRegistrar( catalog, PaleoWorkflowDerivedProjectDir( wf ) );
}

} // namespace paleo::workflow_detail
