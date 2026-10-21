// 层：QGIS 封装
#include "mappingreferencecanvas.h"
#include "mappingreferenceloader.h"
#include "facieshierarchyrenderer.h"
#include "faciesmaplegend.h"
#include "factorstylewriter.h"
#include "mapcanvaslink.h"
#include "mappingartifactwriter.h"
#include "qgisstyleservice.h"
#include "../domain/facieshierarchy.h"
#include "../services/faciesdisplaysettings.h"
#include "../domain/singlefactorrequest.h"
#include <memory>
#include <QResizeEvent>
#include <qgsmapcanvas.h>
#include <qgsmaplayer.h>
#include <qgsmaptoolpan.h>
#include <qgsproject.h>
#include <qgsrasterlayer.h>
#include <qgsrasterrenderer.h>
#include <qgsvectorlayer.h>

MappingReferenceCanvas::MappingReferenceCanvas(QgsMapCanvas *mainCanvas,
                                               QgsProject *project, QWidget *parent)
    : PreviewMapCanvas(parent), m_mainCanvas(mainCanvas)
{
  canvas()->setObjectName(QStringLiteral("referenceCanvas"));
  if (project) {
    canvas()->setProject(project);
    canvas()->mapSettings().setTransformContext(project->transformContext());
  }
  auto *pan = new QgsMapToolPan(canvas());
  pan->setParent(canvas());
  canvas()->setMapTool(pan);
  connect(canvas(), &QgsMapCanvas::scaleChanged, this, &MappingReferenceCanvas::updateLegend);
  connect(&FaciesDisplaySettings::instance(), &FaciesDisplaySettings::thresholdsChanged,
      this, &MappingReferenceCanvas::updateLegend);
}

MappingReferenceCanvas::~MappingReferenceCanvas() { clearReference(); }
bool MappingReferenceCanvas::hasReference() const { return !m_layer.isNull(); }

QgsMapLayer *MappingReferenceLoader::load(const LayerDeclaration &declaration, QgsMapLayer *existing,
                                          const QString &stylesRoot)
{
  std::unique_ptr<QgsMapLayer> layer;
  if (existing) layer.reset(existing->clone());
  else if (declaration.type == QLatin1String("raster"))
    layer = std::make_unique<QgsRasterLayer>(declaration.source, declaration.title);
  else if (declaration.type == QLatin1String("vector"))
    layer = std::make_unique<QgsVectorLayer>(declaration.source, declaration.title, "ogr");
  if (!layer || !layer->isValid()) return nullptr;
  MappingArtifactWriter::restoreRasterCrs(layer.get());
  MappingArtifactWriter::restoreVectorCrs(layer.get());
  if (!declaration.styleRef.isEmpty() && !stylesRoot.isEmpty()) {
    QgisStyleService styles;
    styles.setStylesRoot(stylesRoot);
    styles.applyStyle(layer.get(), declaration.styleRef);
  }
  return layer.release();
}

void MappingReferenceCanvas::setReference(QgsMapLayer *layer, const QVariantMap &metadata)
{
  clearReference();
  if (!layer) return;
  m_layer = layer;
  layer->setParent(canvas());
  setOverrideCrs(layer->crs());
  setLayers({layer});
  m_schema = metadata.value("facies").toList();
  const auto kind = metadata.value("kind").toString();
  // Keep explicit display choices; prediction products retain microfacies
  // when older versions still store the generic automatic mode.
  layer->setCustomProperty("paleo/faciesDisplayMode", FaciesHierarchy::productDisplayMode(kind,
      metadata.value("display_mode", layer->customProperty("paleo/faciesDisplayMode", "auto")).toString()));
  const auto valueSource = metadata.value("value_source").toString();
  const bool analysis = paleo::singlefactor::isAnalysisFactorRaster(kind, valueSource);
  const bool cartographic = paleo::singlefactor::rejectsQuantitativeUse(kind, valueSource);
  m_facies = !kind.startsWith(QLatin1String("constraint")) && !analysis && !cartographic && !m_schema.isEmpty();
  if (analysis || cartographic) {
    m_schema.clear();
    if (auto *raster = qobject_cast<QgsRasterLayer *>(layer)) {
      FactorStyleWriter::applyTo(raster, metadata.value("factor_id").toString());
      if (raster->renderer())
        for (const auto &entry : raster->renderer()->legendSymbologyItems())
          m_schema << QVariantMap{{"name", entry.first}, {"color", entry.second.name()}};
    }
  } else if (kind == QLatin1String("contour_lines")) {
    m_schema.clear();
    FactorStyleWriter::applyContours(qobject_cast<QgsVectorLayer *>(layer));
  }
  zoomToFullExtent();
  // 构造时不联动：独立浏览或隐藏面板换源不应先跳到主图范围。
  m_link = new MapCanvasLink(m_mainCanvas, canvas(), this, false);
  updateLink();
  updateLegend();
}

void MappingReferenceCanvas::clearReference()
{
  delete m_link;
  m_link = nullptr;
  cancelRendering();
  setLayers({});
  delete m_layer.data();
  m_layer = nullptr;
  m_schema.clear();
  m_legendLevel.clear();
  m_facies = false;
  emit legendChanged({});
}

void MappingReferenceCanvas::updateLegend()
{
  if (!m_layer) return;
  auto entries = m_schema;
  if (m_facies) {
    const auto level = FaciesHierarchy::resolveLevel(m_layer->customProperty("paleo/faciesDisplayMode", "auto").toString(),
        canvas()->scale(), FaciesDisplaySettings::instance().thresholds());
    if (m_legendLevel == level) return;
    entries = FaciesHierarchyRenderer::apply(m_layer, m_schema, level);
    m_legendLevel = level;
  }
  emit legendChanged(FaciesMapLegend::symbolEntries(m_layer, entries));
}

void MappingReferenceCanvas::setLinked(bool linked)
{
  m_linked = linked;
  updateLink();
}

void MappingReferenceCanvas::setViewActive(bool active)
{
  m_active = active;
  updateLink();
  if (!active) cancelRendering();
}

void MappingReferenceCanvas::updateLink()
{
  if (m_link) m_link->setEnabled(m_active && m_linked);
}

void MappingReferenceCanvas::resizeEvent(QResizeEvent *event)
{
  PreviewMapCanvas::resizeEvent(event);
  // 原生画布调整大小时保留原比例尺；布局落定后再跟随主图，避免拆分面板
  // 后只对齐中心、参考范围却仍沿用拆分前的宽高比。
  if (m_resizeSyncQueued) return;
  m_resizeSyncQueued = true;
  QMetaObject::invokeMethod(this, [this] {
    m_resizeSyncQueued = false;
    updateLink();
  }, Qt::QueuedConnection);
}
