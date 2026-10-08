// 层：功能
#pragma once
#include "../ai/remotepredictionservice.h"
#include "../catalog/datacatalog.h"
#include "../io/lasdoc.h"
#include "../metadata/layermanifest.h"
#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include <atomic>
#include <memory>
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
  ~MappingWorkbench() override;
  void bindCatalog(DataCatalog *catalog, const QString &projectDir);
  void setPredictionService(RemotePredictionService *service);
  RemotePredictionService *predictionService() const { return m_remote.data(); }
  // 远端预测状态文案（装配根注入；既无服务又无文案时给「未配置」兜底——
  // UI 必须能显示「远端预测未配置，走本地引擎」，不许静默跑替身）。
  void setPredictionStatusHint(const QString &hint);
  QString predictionStatusHint() const {
    return m_predictionHint.isEmpty() && !m_remote
               ? tr("远端预测未配置，走本地引擎")
               : m_predictionHint;
  }
  QVariantList facies(const QString &horizon) const;
  bool saveFacies(const QString &horizon, const QVariantList &items,
                  QString *error);
  QVariantList inputs(const QString &kind) const;
  QVariantList products(const QString &horizon) const;
  CatalogVersion versionForLayer(const QString &id) const;
  LayerDeclaration declaration(const QString &id) const;
  QString layerForVersion(const QString &versionId, QString *error = nullptr);
  QString openWellAttributes(QString *error = nullptr);
  bool predict(const QString &horizon, const QString &kind,
               const QStringList &ids, QString *error,
               const QString &horizonFile = {});
  void cancelPrediction();
  bool busy() const { return !m_request.id.isEmpty(); }
  QString polygonize(const QString &layerId, QString *error);
  QString copyForEditing(const QString &layerId, const QStringList &references,
                         QString *error);
  bool saveEditingVersion(const QString &draftId, QString *error);
  // role: "direction" | "barrier" | "auto"（auto 无法判定时返回错误，
  // 调用侧应询问用户后重试）。
  bool importConstraints(const QString &horizon, const QString &path,
                         const QString &role, QString *error);
  bool importConstraints(const QString &horizon, const QString &path,
                         QString *error) {
    return importConstraints(horizon, path, QStringLiteral("auto"), error);
  }
  // 测区范围/成图边界：面矢量导入为共享置顶层（00_Data，无层位绑定），
  // 返回 true 即已上图；structural_idw 的 boundaryLayerId 从其声明取。
  bool importBoundaryLayer(const QString &path, QString *error);
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
  QString displayMode(const QString &id) const;
  QString resolvedLevel(const QString &id) const;
  QVariantList displayLegend(const QString &id) const;
  bool setDisplayMode(const QString &id, const QString &mode, QString *error);
  void updateDisplayScale(double scale);
  bool selectHierarchyMembers(const QString &id, const QString &level, QString *error);
  bool assignHierarchy(const QString &id, const QList<qint64> &ids,
                       int code, const QString &level, QString *error);
  QList<qint64> selectedFeatures(const QString &id) const;
  QVariantList evidence(const QString &id, const QList<qint64> &ids) const;
  bool addEvidence(const QString &id, const QList<qint64> &ids,
                   const QVariantMap &entry, QString *error);
  bool removeEvidence(const QString &id, const QList<qint64> &ids,
                      const QString &evidenceId, QString *error);
  void styleLayer(const QString &id);
  int labelMode(const QString &id) const;
  bool setLabelMode(const QString &id, int mode, QString *error);
signals:
  void catalogBound();
  void changed();
  void faciesEdited(const QString &layerId);
  void displayChanged(const QString &layerId);
  void faciesChanged(const QString &horizon);
  void predictionBusyChanged(bool busy);
  void predictionProgress(int percent);
  void predictionStatusChanged();
  void productReady(const QString &horizon, const QString &layerId);
  void errorOccurred(const QString &message);

private:
  void synchronizeCatalogLayers();
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
  double m_displayScale = 0;
  bool m_catalogSyncQueued = false;
  RemotePredictionRequest m_request;
  QVariantMap m_predictionParameters;
  std::shared_ptr<std::atomic_bool> m_mockCancelled;
  QString m_predictionHint;
  mutable QString m_logVersion;
  mutable LasDoc m_logCache;
};
