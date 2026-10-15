// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <qgsfeatureid.h>
class QgsMapCanvas;
class QgsMapLayer;
class QgsVectorLayer;
class QgsRubberBand;

// GUI-thread adapter. Its temporary highlight neither selects an edit layer
// nor modifies native feature selection or enters an editing session.
class EvidenceMapAdapter : public QObject {
  Q_OBJECT
public:
  explicit EvidenceMapAdapter(QgsMapCanvas *canvas, QObject *parent = nullptr);
  ~EvidenceMapAdapter() override;
  void bindLayer(QgsVectorLayer *layer);
  QgsVectorLayer *layer() const;
  bool showFeature(const QString &featureUid, bool locate, QString *error = nullptr);
  QgsFeatureId featureId(const QString &featureUid, QString *error = nullptr);
  bool showExtent(bool locate, QString *error = nullptr);
  bool showLayer(QgsMapLayer *layer, bool locate, QString *error = nullptr);
  bool showPoint(double x, double y, const QString &sourceCrsWkt, bool locate,
                 QString *error = nullptr);
  // Owner-thread reads return immutable value snapshots, never QGIS objects.
  // Snapshot: state/scope, feature_uid, WKB, CRS WKT, bounds, geometry_hash,
  // limited semantic attributes and logical layer/version keys.
  QVariantMap featureSnapshot(const QString &featureUid, QString *error = nullptr);
  // Exact intersections after explicit CRS conversion are candidates only.
  // Result: state/scope, features, unresolved, truncated. Vector results are
  // capped at 200 and at most 5*limit bounds candidates are inspected; raster
  // results concern only the extent. No source pixels, support role or proven
  // impact area is inferred. These reads do not alter highlight or selection.
  QVariantMap spatialCandidates(const QVariantMap &target,
      const QString &pinnedUid = {}, int limit = 200, QString *error = nullptr);
  QVariantMap spatialCandidates(QgsMapLayer *source, const QVariantMap &target,
      const QString &pinnedUid = {}, int limit = 200, QString *error = nullptr);
  void clear();
  QString highlightedUid() const { return m_uid; }
  int indexBuildCount() const { return m_indexBuilds; }
signals:
  void featuresSelected(const QString &layerId, const QString &versionId,
                        const QStringList &featureUids);
  void unavailable(const QString &reason);
private:
  void invalidate();
  void rebuildIndex();
  void queueRestore();
  void clearBand();
  bool fail(QString *error, const QString &reason);
  QPointer<QgsMapCanvas> m_canvas;
  QPointer<QgsVectorLayer> m_layer;
  QPointer<QgsRubberBand> m_band;
  QString m_layerId, m_versionId, m_uid;
  QHash<QString, QgsFeatureId> m_index;
  QSet<QString> m_ambiguous;
  bool m_dirty = true;
  quint64 m_generation = 0;
  int m_indexBuilds = 0;
};
