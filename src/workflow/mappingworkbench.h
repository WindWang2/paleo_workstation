// 层：功能
#pragma once
#include "constraintimport.h"
#include "../ai/remotepredictionservice.h"
#include "../catalog/datacatalog.h"
#include "../io/lasdoc.h"
#include "../metadata/layermanifest.h"
#include "../qgis/composeattribution.h"
#include "../qgis/mappingartifactwriter.h"
#include "../services/evidenceindex.h"
#include "derivedassets.h"
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QHash>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <memory>
class QgisLayerService;
class QgisProcessingService;
class QgisProjectService;
class QgsFeature;
class QgsMapLayer;
class ConstraintWorkflow;
class PaleoTask;
class PaleoTaskService;
class WellPredictionService;
namespace paleo::jobs {
template <class JobT> class JobRunner;
}

// X16 统一冻结输入解析结果：三处旧实现（composeAttribution / At / impact）
// 已合一，口径见 resolveFrozenCompose。unresolved 为 {layer_id,version_id,
// reason} 列表；inputs 带保活句柄，持有一整次重放/对比期间缓存不驱逐。
struct FrozenCompose {
  QString composedVersionId, horizon;
  QVariantList facies;
  QList<double> thresholds;
  paleo::attribution::FrozenGrid grid;
  QList<paleo::attribution::ResolvedInput> inputs;
  QVariantList unresolved;
};

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
  // 只读目录访问（影响分析/反查等 UI 编排用）。
  DataCatalog *catalog() const { return m_catalog; }
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
  // 未登记的残留图件仅移除声明和工程图层，保留所有数据文件。
  // 级联删除需由 UI 展示 removalProducts 的完整清单并取得确认。
  QVariantList removalProducts(const QString &layerId, QString *error) const;
  bool removeProduct(const QString &layerId, QString *error,
                     bool includeDerived = false);
  // role: "direction" | "barrier" | "auto"（auto 无法判定时返回错误，
  // 调用侧应询问用户后重试）。
  bool importConstraints(const QString &horizon, const QString &path,
                         const QString &role, QString *error);
  bool startConstraintImport(const QString &horizon, const QString &path, const QString &role,
                             QString *error);
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
  bool startCompose(const QString &horizon, const QStringList &layerIds,
                    const QVariantMap &params, QString *error);
  bool startFaciesConsistency(const QString &layerId, QString *error,
                              const QString &counterpartId = {});
  QString compose(const QString &horizon, const QStringList &layerIds,
                  const QVariantMap &params, QString *error);
  QString snapshotConstraints(const QString &horizon,
                              const QStringList &parents, QString *error);
  QVariantList wellPredictions(const QString &layerId) const;
  bool isWellPrediction(const QString &layerId) const;
  LasDoc predictionLog(const QString &versionId) const;
  bool assignFacies(const QString &draftId, const QList<qint64> &featureIds,
                    int code, QString *error);
  bool reviseWellInterval(const QString &id, const QString &wellId,
                          int index, int code, QString *error);
  QString displayMode(const QString &id) const;
  QString resolvedLevel(const QString &id) const;
  QVariantList displayLegend(const QString &id) const;
  bool setDisplayMode(const QString &id, const QString &mode, QString *error);
  // Per-polygon override; "auto" follows the layer's current display level.
  QString featureDisplayMode(const QString &id, qint64 featureId) const;
  bool isFaciesPolygon(const QString &id) const;
  bool setFeatureDisplayMode(const QString &id, const QList<qint64> &ids,
                             const QString &mode, QString *error);
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
  // View snapshot: immutable values only; old/malformed data is explicit.
  QVariantMap featureEvidence(const QString &id, const QList<qint64> &ids) const;
  // 图斑级计算归因：用成果保存时冻结的输入版本重放确定性融合决策。
  // 返回值对象（status ok/unresolved/manual + inputs[] 决策分布）；不做任何
  // 地质支持关系推断。layerId 为相面成果或其编辑副本；f 为选中图斑。
  QVariantMap composeAttribution(const QString &layerId, const QgsFeature &feature) const;
  // 局部位置级（Level 3）：在图斑内指定点重放单点决策；point 为图层 CRS 坐标。
  QVariantMap composeAttributionAt(const QString &layerId, const QgsFeature &feature,
                                   const QPointF &point) const;
  // G05 证据变化空间影响分析。change = {kind:"input_version"|"constraint_edited"|"other",
  //   before_version_id, after_version_id, horizon, constraint_ids?, detail?}。
  // 三级分明：direct_products（计算血缘事实）/ potential_products（派生链可达、
  //   无法局部证明）/ determined（像元级重放 diff + 受影响图斑）。分析基于
  //   before/after 版本快照（不建立在当前活动图层上），报告 JSON 落
  //   artifacts/reports/，返回 map 带 report_path。
  QVariantMap analyzeImpact(const QVariantMap &change, QString *error);
  // G09 受控重算闭环。
  // recomputePlan：从影响分析结果生成重算计划（value object，不执行）。
  //   每条 entry：{target_version, horizon, mode:"local_rerun"|"whole_rerun",
  //   inputs:[{layer_id,frozen_version,current_version}], parameters(真实执行参数),
  //   protection:{confirmed_features, needs_review_features}}。
  //   确定性像元 diff 覆盖的成果给 local_rerun；无法局部化的给 whole_rerun，
  //   不伪装成局部重算。
  QVariantMap recomputePlan(const QVariantMap &impactResult, QString *error);
  // executeRecompute：按计划调用现有综合编图工作流（同参数、当前输入版本）
  //   → 生成新成果版本，绝不覆盖历史版本；已发布版本不动。返回每条 entry
  //   的执行结果（新版本 id 或失败原因）。确认过的人工解释保留在旧版本里
  //   （immutable），需要迁移时由人工复核决定。
  QVariantMap executeRecompute(const QVariantMap &plan, QString *error);
  // compareVersions：新旧成果版本对比（G09 验收）——栅格像元级 diff（变化
  //   像元数/相编码转移矩阵）+ 相面统计；几何匹配按像元中心归属做 best-effort
  //   并如实标注方法。返回 {method, cells_changed, total_cells, code_matrix,
  //   polygon_counts:{before,after}, note}。坐标系不同的版本拒绝像元级对比。
  QVariantMap compareVersions(const QString &oldVersionId, const QString &newVersionId,
                              QString *error);
  // X13：从视图迁入的版本影响入口——所选图件为变化后版本，同资产的上一版
  // 为变化前；单一版本如实报错。
  QVariantMap analyzeLayerVersionImpact(const QString &layerId,
                                        const QString &horizon, QString *error);
  // X13：受控重算产物与它所替代的旧版本做像元级对比（recompute_provenance.
  // recompute_of）。非重算产物如实报错。报告 JSON 落 artifacts/reports/，
  // 返回 map 带 report_path。
  QVariantMap compareWithRecomputeSource(const QString &layerId, QString *error);
  // G08/G12 证据反向检索：name ∈ {"pending_review","contradicted",
  // "references_source","stale_sources","search"}；argument = 层位 / 来源版本
  // id / 检索词。索引同步在有任务服务时走后台（startEvidenceIndexSync），
  // 查询永远直接打现有索引文件并按 meta 如实上报新鲜度：
  // meta = {index_state:"fresh"|"syncing"|"stale"|"error", index_error,
  //         total, shown}。行新增 unlinked / relation_state（active|history）/
  // is_current_version / hit_reason。原始证据仍以 GPKG 属性为唯一事实源。
  QVariantList evidenceReverseQuery(const QString &name, const QString &argument,
                                    QString *error, QVariantMap *meta = nullptr);
  // X9：后台证据索引同步。任务服务在场 → JobRunner 异步（worker 线程内建用
  // 销独立 QSQLITE 连接，不跨线程复用）；无任务服务 → 同一代码路径在当前
  // 线程同步执行（测试壳/shell）。同步完成的版本修订记在
  // m_evidenceIndexSyncedRevision，与 catalogRevision() 对账得出索引新鲜度。
  bool startEvidenceIndexSync(QString *error);
  QString localOperator() const;
  bool changeEvidence(const QString &id, const QList<qint64> &ids,
                      const QString &action, const QVariantMap &parameters,
                      QString *error);
  void styleLayer(const QString &id);
  // facies-ux 4：地震相 × 测井相预测一致性。layerId 为任一侧（测井相点或
  // 地震相栅格/相面），另一侧自动取同层位最新结果（工作副本优先）；
  // counterpartId 非空时显式指定。逐井在井位取地震相，比对测井相编码，
  // 出逐井表 + 混淆矩阵 + 一致率/κ，报告 JSON 原子写入
  // artifacts/reports/（返回 map 带 report_path / well_layer / seismic_layer）。
  QVariantMap faciesConsistency(const QString &layerId, QString *error,
                                const QString &counterpartId = {});
  // 可参与一致性比对的同层位候选（{id,name,family:"well"|"seismic"}）。
  QVariantList consistencyCandidates(const QString &horizon) const;
  int labelMode(const QString &id) const;
  bool setLabelMode(const QString &id, int mode, QString *error);
signals:
  void consistencyReady(const QVariantMap &report);
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
  // 证据索引后台同步终态（state 含 index_state/index_error/stats）。
  void evidenceIndexSynced(const QVariantMap &state);
  void errorOccurred(const QString &message);

private:
  QVariantMap resolveEvidenceSource(const QVariantMap &record, const QVariantMap &spatialTarget,
                                    bool loadSavedSource, QSet<QString> *spatialSources,
                                    QHash<QString, QVariantMap> *spatialResults = nullptr) const;
  // 归因：从成果版本链定位 composed_facies 计算版本（广度优先、深度受限），
  // 找不到返回空 id（纯人工图斑走 manual 语义）。
  QString composedAncestorVersion(const QString &layerId) const;
  // 冻结输入版本 → 独立打开的采样图层（有界 LRU；随目录上下文重置）。
  // 返回 shared_ptr 保活：LRU 驱逐只丢缓存引用，调用方持有的句柄仍有效
  //（X1：旧裸指针 + QHash 任意驱逐会在一次操作中途悬垂）。
  std::shared_ptr<QgsMapLayer> attributionLayer(const QString &versionId) const;
  // 统一冻结输入解析（X16）：substitute 提供 versionId→新版本替换（影响
  // 分析注入变化后的版本）。填充 grid/inputs/unresolved；返回是否全部可解。
  bool resolveFrozenCompose(const QString &composedVersionId,
                            const QHash<QString, QString> &substitute,
                            FrozenCompose *out) const;
  void connectPredictionService(RemotePredictionService *service);
  void publishFactorAlias(const QString &horizon, const QString &id);
  void synchronizeCatalogLayers();
  bool publishConstraintImport(const QString &horizon, const QString &inputPath,
                               const QString &sourcePath,
                               const QVector<paleo::ImportedConstraintRecord> &records,
                               QString *error, const DerivedFileFingerprint *fingerprint = nullptr,
                               const QVariantMap &importMeta = {});
  void ensureVersionIndex() const;
  bool ready(const QString &horizon, QString *error) const;
  QString schemaVersion(const QString &horizon) const;
  QString record(const QString &path, const QString &horizon,
                 const QString &kind, const QString &title, const QString &type,
                 const QStringList &parents, QVariantMap extra, QString *error,
                 const QString &suffix = QString(),
                 const QString &assetKey = QString(),
                 const DerivedFileFingerprint *fingerprint = nullptr);
  QString ensureInputVersion(const QString &layerId, QString *error);
  bool recordInterpretationEdit(const QString &id, qint64 fid,
                                int beforeCode, int afterCode, QString *error);
  bool recordSpatialInterpretationEdit(const QString &id, qint64 fid,
                                      const QString &kind, QString *error);
public:
  struct ComposeJob {
    ~ComposeJob();
    QString horizon, dir, outputPath, error, productId;
    QStringList ids, parents;
    QVariantList sources, facies;
    QList<double> thresholds;
    QVariantMap params;
    DerivedFileFingerprint fingerprint;
    MappingArtifactWriter::ComposeReceipt receipt; // 每输入胜出像元回执
    QPointer<DataCatalog> catalog;
  };
  bool prepareCompose(const QString &horizon, const QStringList &ids,
                      const QVariantMap &params, ComposeJob &job,
                      QString *error);
  static bool computeCompose(ComposeJob &job,
                             const std::function<bool()> &cancel,
                             const std::function<void(double)> &progress);
  bool commitCompose(ComposeJob &job, QString *error);
  // 转面作业快照（纯数据，跨线程安全；compute 不读成员）。
  struct PolygonizeJob {
    ~PolygonizeJob();
    QString sourceLayerId, horizon, title, rasterUri, outputPath, parent, dir;
    QString crsWkt; // 声明层 CRS（数据源无 CRS 时 worker 侧补齐）
    QVariantList facies;
    QString valueRanges; // 分级值域 JSON（连续单因素来源才有；写相面 value_min/max）
    QVariant mock, provenance; // 算法 PROVENANCE 回执（commit 时入版本 extra）
    DerivedFileFingerprint fingerprint;
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
  // 证据索引后台同步作业快照（纯数据；compute 在 worker 线程用本地 Index）。
  struct IndexSyncJob {
    QString dir;
    QVector<CatalogVersion> versions;
    int revision = 0;              // 快照时的 catalogRevision
    QPointer<DataCatalog> catalog; // commit 时校验工程未切换
    QString error;
    int changed = -1;
    QVariantMap stats;
  };
  // 三段式各段：compute 在 worker 线程建用销本地 Index；commit 在 GUI 线程
  // 校验工程未切换后登记修订并 emit evidenceIndexSynced。静态 + 成员，供
  // 无任务服务时的同步内联路径与 JobRunner 回调共用同一实现。
  static bool computeIndexSync(
      IndexSyncJob &job, const std::function<bool()> &cancel,
      const std::function<void(double, const QString &)> &progress);
  bool commitIndexSync(IndexSyncJob &job, QString *error);
  QVariantMap consistencyImpl(const QString &layerId, QString *error,
                              const QString &counterpartId, bool background);
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
  std::unique_ptr<paleo::jobs::JobRunner<ComposeJob>> m_composeRunner;
  QString m_predictionHint;
  mutable QString m_logVersion;
  mutable LasDoc m_logCache;
  mutable bool m_versionIndexValid = false, m_productsValid = false;
  mutable quint64 m_versionIndexSeq = 0;
  mutable int m_versionIndexRevision = 0;
  mutable QHash<QString, CatalogVersion> m_aliasVersions, m_pathVersions;
  // Owner-thread QGIS adapters; bounded and reset with the catalog context.
  mutable QHash<QString, QPointer<QObject>> m_evidenceSpatialAdapters;
  // 冻结输入版本的采样图层（图斑归因用；有界 LRU，bindCatalog 重置）。
  // 缓存只占一份引用；重放期间的有效期由 ResolvedInput::hold 保证。
  mutable QHash<QString, std::shared_ptr<QgsMapLayer>> m_attributionLayers;
  mutable QStringList m_attributionLru;
  // G08 反向检索索引（可重建 sidecar；bindCatalog 重置）。GUI 侧只读
  // 连接；写由 IndexSyncJob 在 worker 线程的独立连接完成（WAL 共存）。
  std::unique_ptr<paleo::evidenceindex::Index> m_evidenceIndex;
  std::unique_ptr<paleo::jobs::JobRunner<IndexSyncJob>> m_indexSyncRunner;
  // 已同步覆盖的目录修订 / 最近一次失败（或取消）的修订与错误。
  int m_evidenceIndexSyncedRevision = -1;
  int m_evidenceIndexAttemptedRevision = -1;
  QString m_evidenceIndexSyncError;
  QVariantMap m_evidenceIndexStats;
  mutable QVariantList m_productsCache;
};
