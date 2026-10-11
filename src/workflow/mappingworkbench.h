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
#include <functional>
#include <memory>
class QgisLayerService;
class QgisProcessingService;
class QgisProjectService;
class ConstraintWorkflow;
class PaleoTask;
class PaleoTaskService;
class WellPredictionService;
namespace paleo::jobs {
template <class JobT> class JobRunner;
}

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
  void setWellPredictionService(WellPredictionService *service);
  QString wellPredictionStatusHint() const;
  RemotePredictionService *predictionService() const { return m_remote.data(); }
  // 智能预测进任务中心：每次预测一行（进度/取消/终态），装配根注入；
  // 未注入时预测照常运行，只是不出任务行。
  void setTaskService(PaleoTaskService *tasks);
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
  // 地层优势相矢量表（welllog.formations）：与数据管理的逐组预测同一张表。
  QString openWellFormations(QString *error = nullptr);
  // 测井基础图层保证在场（井位 + 岩性相属性 + 地层相属性 + 井点单因素，
  // 产物文件存在才物化——「如果有的话」）。编图链三页进入与测井预测
  // 落盘后调用：工程打开时文件尚不存在的首预测，从此无需重开工程。
  void ensureWellBaseLayers();
  bool predict(const QString &horizon, const QString &kind,
               const QStringList &ids, QString *error,
               const QString &horizonFile = {});
  void cancelPrediction();
  bool busy() const { return !m_request.id.isEmpty(); }
  // 同步转面（无任务服务的壳/测试；自动编图在未注入任务服务时也走这里）。
  QString polygonize(const QString &layerId, QString *error);
  // 相栅格转面后台任务（任务中心可见、可取消）：prepare 在主线程做与同步
  // 路径相同的校验并冻结输入（栅格 URI、父版本、相分类、工程目录），worker
  // 上独立建层跑 paleo:paleo_facies_polygonize 并同步相属性，commit 回主线程
  // 登记版本 + 声明图层（catalog 写线程纪律，见审查 UIS-12）。工程切换或
  // 重绑目录后完成的旧结果按过期丢弃。成功发 productReady（与同步路径同一
  // 收尾）；返回 false = 未启动（error 给因，含「已有转面任务」）。
  bool startPolygonize(const QString &layerId, QString *error);
  bool polygonizeBusy() const;
  void cancelPolygonize();
  QString copyForEditing(const QString &layerId, const QStringList &references,
                         QString *error);
  bool saveEditingVersion(const QString &draftId, QString *error);
  // 永久删除生成的图件/编辑副本（product.*/draft.* 才受理）：下架该资产
  // 全部图层声明、删 catalog 资产与受管文件、清草稿工作目录。
  // 级联删除需由 UI 展示 removalProducts 的完整清单并取得确认。
  QVariantList removalProducts(const QString &layerId, QString *error) const;
  bool removeProduct(const QString &layerId, QString *error,
                     bool includeDerived = false);
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
  bool isWellPrediction(const QString &layerId) const;
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
  void predictionDetailChanged(const QString &detail);
  void predictionStatusChanged();
  void productReady(const QString &horizon, const QString &layerId);
  // 后台转面终态（成功时 productId 非空；取消/过期 cancelled=true）。
  void polygonizeFinished(const QString &sourceLayerId,
                          const QString &productId, const QString &error,
                          bool cancelled);
  void errorOccurred(const QString &message);

private:
  void connectPredictionService(RemotePredictionService *service);
  void synchronizeCatalogLayers();
  void ensureVersionIndex() const;
  bool ready(const QString &horizon, QString *error) const;
  QString schemaVersion(const QString &horizon) const;
  QString record(const QString &path, const QString &horizon,
                 const QString &kind, const QString &title, const QString &type,
                 const QStringList &parents, QVariantMap extra, QString *error,
                 const QString &suffix = QString(),
                 const QString &assetKey = QString());
  QString ensureInputVersion(const QString &layerId, QString *error);
public:
  // 转面作业快照（纯数据，跨线程安全；compute 不读成员）。
  struct PolygonizeJob {
    QString sourceLayerId, horizon, title, rasterUri, outputPath, parent, dir;
    QVariantList facies;
    QVariant mock;
    QPointer<DataCatalog> catalog;
    QString productId; // commit 产出
    QString error;
  };
  // 三段式各段（public 便于测试直驱；UI 走 startPolygonize）。
  bool preparePolygonize(const QString &layerId, PolygonizeJob *job,
                         QString *error);
  static bool computePolygonize(PolygonizeJob &job,
                                const std::function<bool()> &cancelled,
                                const std::function<void(double)> &progress,
                                QgisProcessingService *processing);
  bool commitPolygonize(PolygonizeJob &job, QString *error);

private:
  void finishPrediction(const RemotePredictionResult &result);
  // 任务中心行：开一行 / 收尾（error 空 = 完成）/ 取消（取消态收尾）。
  void beginPredictionTask(const QString &title);
  void endPredictionTask(const QString &error = {});
  void cancelPredictionTask();
  // 测井相点的井段 → 每井每个地层单位（组/段）的优势相行；地层单位取自该井
  // tops（顶 = 本层位顶，底 = 下一层位顶，最末取预测底）。predicted=false 为
  // 人工修订：只更新维护的优势相，不动「预测优势相」。
  QVariantList wellFormationUpdates(const QVariantList &points,
                                    const QVariantList &schema, bool predicted,
                                    const QString &method = {},
                                    const QString &jobId = {}) const;
  QgisLayerService *m_layers;
  QgisProcessingService *m_processing;
  QgisProjectService *m_project;
  ConstraintWorkflow *m_constraints;
  QPointer<DataCatalog> m_catalog;
  QPointer<RemotePredictionService> m_remote;
  QPointer<WellPredictionService> m_wellRemote;
  QString m_dir;
  double m_displayScale = 0;
  bool m_catalogSyncQueued = false;
  RemotePredictionRequest m_request;
  QVariantMap m_predictionParameters;
  std::shared_ptr<std::atomic_bool> m_mockCancelled;
  // 测井预测的取数阶段（后台读 LAS 深度范围，读完才提交预测服务）。
  std::shared_ptr<std::atomic_bool> m_prepareCancelled;
  QPointer<PaleoTaskService> m_taskSvc;
  QPointer<PaleoTask> m_task;
  std::unique_ptr<paleo::jobs::JobRunner<PolygonizeJob>> m_polygonizeRunner;
  QString m_predictionHint;
  mutable QString m_logVersion;
  mutable LasDoc m_logCache;
  mutable bool m_versionIndexValid = false, m_productsValid = false;
  mutable quint64 m_versionIndexSeq = 0;
  mutable int m_versionIndexRevision = 0;
  mutable QHash<QString, CatalogVersion> m_aliasVersions, m_pathVersions;
  mutable QVariantList m_productsCache;
};
