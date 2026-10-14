// 层：QGIS 封装
#pragma once
#include <QObject>
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QStringList>
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
