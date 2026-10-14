// 层：功能
#include "wellmaplink.h"

#include "selectioncontext.h"

#include <QSignalBlocker>
#include <QTimer>

#include <qgsfeature.h>
#include <qgsfeatureiterator.h>
#include <qgsfeaturerequest.h>
#include <qgsfields.h>
#include <qgsmapcanvas.h>
#include <qgsvectorlayer.h>

// §31 well–map linkage.
//
// Direction A (layer -> ctx): hooked on the layer's own selectionChanged —
// it fires for canvas picks and any other selection source, and reports the
// delta while ctx wants the full state, so selectedFeatureIds() is read as
// authoritative. fids resolve to well ids through m_idField, then broadcast
// with origin "canvas". The ctx->broadcasting() check swallows the signal
// emitted by our own direction-B apply mid-broadcast.
//
// Direction B (ctx -> layer): foreign origins re-select matching features.
// QSignalBlocker on the layer is the reentrancy guard: selectByIds()'s
// selectionChanged cannot re-enter ctx, so the classic A->B->A ping-pong
// never starts. triggerRepaint() restores the visual update the blocker
// suppressed (the canvas relies on layer->selectionChanged to repaint).

WellMapLink::WellMapLink(QgsMapCanvas *canvas, SelectionContext *ctx, QObject *parent)
  : QObject(parent), m_canvas(canvas), m_ctx(ctx)
{
  if (m_ctx)
    connect(m_ctx, &SelectionContext::selectionChanged,
            this, &WellMapLink::onContextSelection);
}

void WellMapLink::setWellLayer(QgsVectorLayer *layer, const QString &idField)
{
  if (m_layer == layer && m_idField == idField)
    return;
  if (m_layer)
    disconnect(m_layer, nullptr, this, nullptr); // drop direction-A hook on old layer

  m_layer = layer;
  m_idField = idField;
  m_identityIndex.clear();
  m_indexDirty = true;
  if (!m_layer)
    return;

  connect(m_layer, &QgsVectorLayer::selectionChanged, this,
          [this](const QgsFeatureIds &, const QgsFeatureIds &, bool) {
            if (!m_ctx || !m_layer)
              return;
            if (m_ctx->broadcasting())
              return; // echo of a direction-B apply still in flight — swallow
            const int idx = m_layer->fields().indexOf(m_idField);
            if (idx < 0)
              return;

            const QgsFeatureIds sel = m_layer->selectedFeatureIds();
            if (sel.isEmpty())
            {
              m_ctx->clear(QStringLiteral("well_map"));
              return;
            }

            QgsFeatureRequest req;
            req.setFilterFids(sel);
            req.setSubsetOfAttributes(QgsAttributeList{idx});
            req.setFlags(Qgis::FeatureRequestFlag::NoGeometry);

            QStringList wellIds;
            QgsFeature f;
            QgsFeatureIterator it = m_layer->getFeatures(req);
            while (it.nextFeature(f))
              wellIds << f.attribute(idx).toString();
            m_ctx->setSelection(wellIds, QStringLiteral("well_map"));
          });
  connect(m_layer, &QgsVectorLayer::featureAdded, this, &WellMapLink::invalidateIndex);
  connect(m_layer, &QgsVectorLayer::featureDeleted, this, &WellMapLink::invalidateIndex);
  connect(m_layer, &QgsVectorLayer::updatedFields, this, &WellMapLink::invalidateIndex);
  connect(m_layer, &QgsVectorLayer::attributeValueChanged, this,
          [this](QgsFeatureId, int field, const QVariant &) {
    if (m_layer && field == m_layer->fields().indexOf(m_idField)) invalidateIndex();
  });
  connect(m_layer, &QgsVectorLayer::editingStopped, this, &WellMapLink::invalidateIndex);
  connect(m_layer, &QgsVectorLayer::afterRollBack, this, &WellMapLink::invalidateIndex);
  const auto restore = [this] {
    invalidateIndex();
    if (m_restorePending) return;
    m_restorePending = true;
    QTimer::singleShot(0, this, [this] {
      m_restorePending = false;
      if (m_ctx) onContextSelection(m_ctx->selectedIds(), QStringLiteral("rebind"));
    });
  };
  connect(m_layer, &QgsMapLayer::dataChanged, this, restore);
  connect(m_layer, &QgsMapLayer::dataSourceChanged, this, restore);
  if (m_ctx) onContextSelection(m_ctx->selectedIds(), QStringLiteral("rebind"));
}

void WellMapLink::invalidateIndex() { m_indexDirty = true; }
void WellMapLink::rebuildIndex() {
  if (!m_indexDirty || !m_layer) return;
  m_identityIndex.clear();
  const int index = m_layer->fields().indexOf(m_idField);
  if (index < 0) return;
  QgsFeatureRequest request;
  request.setSubsetOfAttributes({index});
  request.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
  auto features = m_layer->getFeatures(request);
  QgsFeature feature;
  while (features.nextFeature(feature))
    m_identityIndex[feature.attribute(index).toString()].insert(feature.id());
  m_indexDirty = false;
}

void WellMapLink::onContextSelection(const QStringList &ids, const QString &origin)
{
  if (!m_layer || origin == QLatin1String("well_map"))
    return; // nothing to sync, or our own broadcast coming back around

  const int idx = m_layer->fields().indexOf(m_idField);
  if (idx < 0)
    return;

  // Build the stable-ID index once per provider/edit revision, not per click.
  rebuildIndex();
  QgsFeatureIds fids;
  for (const auto &identity : ids) fids.unite(m_identityIndex.value(identity));

  {
    const QSignalBlocker blocker(m_layer); // reentrancy guard — no echo into ctx
    m_layer->selectByIds(fids);
  }
  m_layer->triggerRepaint();
}

QgsVectorLayer *WellMapLink::wellLayer() const
{
  return m_layer.data();
}
