// 层：功能
#pragma once
#include "../ai/remotepredictionservice.h"
#include "../catalog/datacatalog.h"
#include "../io/lasdoc.h"
#include "../metadata/layermanifest.h"
#include <QObject>
#include <QPointer>
#include <QVariantMap>
class QgisLayerService;
class QgisProcessingService;
class QgisProjectService;
class ConstraintWorkflow;

// One project-bound catalog writer; all QGIS/catalog mutations run on its GUI
// thread.
class MappingWorkbench : public QObject {
  Q_OBJECT
public:
  MappingWorkbench(QgisLayerService *layers, QgisProcessingService *processing,
                   QgisProjectService *project, ConstraintWorkflow *constraints,
                   QObject *parent = nullptr);
  void bindCatalog(DataCatalog *catalog, const QString &projectDir);
  void setPredictionService(RemotePredictionService *service);
  QVariantList facies(const QString &horizon) const;
  bool saveFacies(const QString &horizon, const QVariantList &items,
                  QString *error);
  QVariantList inputs(const QString &kind) const;
  QVariantList products(const QString &horizon) const;
  CatalogVersion versionForLayer(const QString &id) const;
  LayerDeclaration declaration(const QString &id) const;
  bool predict(const QString &horizon, const QString &kind,
               const QStringList &ids, QString *error);
  void cancelPrediction();
  bool busy() const { return !m_request.id.isEmpty(); }
  QString polygonize(const QString &layerId, QString *error);
  QString copyForEditing(const QString &layerId, const QStringList &references,
                         QString *error);
  bool saveEditingVersion(const QString &draftId, QString *error);
  bool importConstraints(const QString &horizon, const QString &path,
                         QString *error);
  bool generateFactor(const QString &horizon, const QString &factorId,
                      const QVariantMap &params, QString *error);
  bool generateContours(const QString &horizon, const QString &layerId,
                        double interval, QString *error);
  QString compose(const QString &horizon, const QStringList &layerIds,
                  const QVariantMap &params, QString *error);
  QString snapshotConstraints(const QString &horizon,
                              const QStringList &parents, QString *error);
  QVariantList wellPredictions(const QString &layerId) const;
  LasDoc predictionLog(const QString &versionId) const;
  bool assignFacies(const QString &draftId, const QList<qint64> &featureIds,
                    int code, QString *error);
  bool reviseWellInterval(const QString &draftId, const QString &wellId,
                          int interval, int code, QString *error);
  void styleLayer(const QString &id);
  int labelMode(const QString &id) const;
  bool setLabelMode(const QString &id, int mode, QString *error);
signals:
  void changed();
  void faciesEdited(const QString &layerId);
  void faciesChanged(const QString &horizon);
  void predictionBusyChanged(bool busy);
  void predictionProgress(int percent);
  void productReady(const QString &horizon, const QString &layerId);
  void errorOccurred(const QString &message);

private:
  bool ready(const QString &horizon, QString *error) const;
  QString schemaVersion(const QString &horizon) const;
  QString record(const QString &path, const QString &horizon,
                 const QString &kind, const QString &title, const QString &type,
                 const QStringList &parents, QVariantMap extra, QString *error,
                 const QString &suffix = QString(),
                 const QString &assetKey = QString());
  QString ensureInputVersion(const QString &layerId, QString *error);
  void finishPrediction(const RemotePredictionResult &result);
  QgisLayerService *m_layers;
  QgisProcessingService *m_processing;
  QgisProjectService *m_project;
  ConstraintWorkflow *m_constraints;
  QPointer<DataCatalog> m_catalog;
  QPointer<RemotePredictionService> m_remote;
  QString m_dir;
  RemotePredictionRequest m_request;
  bool m_importing = false;
  mutable QString m_logVersion;
  mutable LasDoc m_logCache;
};
