// 层：功能
#include "wellmaplink.h"

#include "selectioncontext.h"

#include <QSignalBlocker>

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
              m_ctx->clear(QStringLiteral("canvas"));
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
            m_ctx->setSelection(wellIds, QStringLiteral("canvas"));
          });
}

void WellMapLink::onContextSelection(const QStringList &ids, const QString &origin)
{
  if (!m_layer || origin == QLatin1String("canvas"))
    return; // nothing to sync, or our own broadcast coming back around

  const int idx = m_layer->fields().indexOf(m_idField);
  if (idx < 0)
    return;

  // Resolve well ids -> feature ids via the id-field index.
  QgsFeatureIds fids;
  QgsFeatureRequest req;
  req.setSubsetOfAttributes(QgsAttributeList{idx});
  req.setFlags(Qgis::FeatureRequestFlag::NoGeometry);
  QgsFeature f;
  QgsFeatureIterator it = m_layer->getFeatures(req);
  while (it.nextFeature(f))
    if (ids.contains(f.attribute(idx).toString()))
      fids.insert(f.id());

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
