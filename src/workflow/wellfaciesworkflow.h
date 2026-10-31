// 层：功能
#pragma once
#include "ai/wellfaciesservice.h"
#include "../services/paleotaskservice.h"
#include <QPointer>
#include <QVariantList>
#include "domain/stratigraphicunit.h"
class DataCatalog;
class QgisLayerService;
class QgsVectorLayer;
// 测井相预测按地层组进行：井上每个有模型的地层组各提交一次（模型取
// formationGroup 与该组一致的那个），逐组顺序执行、逐组返回——每组一回来
// 就并入预测相道显示并按组缓存；全部组走完后一次性回写属性。注入任务服务
// 后每个地层组是任务中心里的一条任务（进度条 + 完成/失败/已取消状态，可单独取消）。
class WellFaciesWorkflow : public QObject {
  Q_OBJECT
public:
  explicit WellFaciesWorkflow(QObject *parent = nullptr);
  ~WellFaciesWorkflow() override;
  WellFaciesConfig config() const { return m_config; }
  void configure(const WellFaciesConfig &config, bool persist = true);
  void setData(const WellComposite::ComprehensiveWellData &data);
  void selectModel(const QString &id);
  void refreshModels();
  void run();
  void setAnalysisUnit(const paleo::strat::UnitSelection &unit);
  void cancel();
  // 绑定图层服务时预测回写可维护矢量相属性；未绑定保留旧资产兼容缝。
  // 方向 69：旧接口落 catalog DERIVED 解释岩性资产（well_litho_intervals
  // schema 1）+ per-well interpretation 链接。projectDir 空 = 按 catalog
  // 当前工程解析（换工程自适应）。未绑定/不可写 → completed 仍调
  // publishLithoAsset，失败原因如实进状态行（不静默跳过）。
  void setCatalog(DataCatalog *catalog, const QString &projectDir = QString());
  void setLayerService(QgisLayerService *layers);
  // 注入后逐组预测各进任务中心（非 quiet：数据页会自动露出底栏任务页）；
  // 不注入（测试/无任务面）则只走状态行。
  void setTaskService(PaleoTaskService *tasks);
  void requestAttributeTable(bool factors = false);
  // 打开地层优势相矢量表（welllog.formations）；与智能预测页同一张表。
  void requestFormationTable();
  void refreshAttributes();
  // 预测结果 → DERIVED 资产（与 completed 信号同一条生产者路；测试直达缝）。
  // 成功返回空串；未登记返回如实原因（无 catalog/井名未解析/无有效段/
  // 登记失败）——失败不写资产（诚实面）。
  QString publishLithoAsset(const WellFaciesResult &result);
signals:
  void modelsChanged(const QVariantList &models);
  // 逐组计划摘要（空 = 当前不可预测）；先于 availabilityChanged 发出。
  void planChanged(const QString &summary);
  void availabilityChanged(bool ready, const QString &reason);
  void busyChanged(bool busy);
  void statusChanged(const QString &message);
  void resultReady(const WellFaciesResult &result);
  void resultCleared();
  void attributeAvailabilityChanged(bool available, const QString &reason);
  void attributesReady(const WellComposite::ComprehensiveWellData &data);
  void factorMaintenanceRequested();
  void attributeTableRequested(const QString &layerId);

private:
  bool scopePlanToUnit(QString *error);
  QString publishUnitResult(const WellFaciesResult &result);
  paleo::strat::UnitSelection m_analysisUnit;
  bool m_hasAnalysisContext = false;
  QStringList m_unitParentVersions;
  void updateInput(bool restoreCache = true);
  void loadKeyFromKeychain();
  int m_configGeneration = 0; // configure() 递增：迟到的钥匙串读结果不覆盖新配置
  QString cachePath(const WellFaciesRun &run) const;
  QString projectDir() const;
  QString publishAttributes(const WellFaciesResult &result);
  WellFaciesResult withDominance(WellFaciesResult result) const;
  QString planSummary() const;
  QString runLabel(const WellFaciesRun &run) const;
  void startNextRun();
  void onGroupCompleted(const WellFaciesResult &part);
  void onGroupFailed(const QString &reason);
  void onTaskCancelAsked(PaleoTask *task);
  void finishTask(int index, const QString &error = QString());
  void abortRun();
  void finishRun();
  WellFaciesService m_service;
  WellFaciesConfig m_config;
  WellComposite::ComprehensiveWellData m_data, m_sourceData;
  QPointer<QgisLayerService> m_layers;
  QPointer<QgsVectorLayer> m_attributeLayer;
  QPointer<PaleoTaskService> m_taskSvc;
  QVector<WellFaciesModel> m_models;
  QString m_modelId, m_modelError;
  WellFaciesPlan m_plan;
  // 逐组运行状态：m_runs 为本次提交的各组，m_runIndex 为当前组（-1 = 未开始）；
  // m_runTasks 与 m_runs 一一对应，终态已上报的置空。
  QVector<WellFaciesRun> m_runs;
  QVector<QPointer<PaleoTask>> m_runTasks;
  QVector<bool> m_runSkip; // 排队中被取消的组：轮到时直接跳过
  QVector<WellFaciesResult> m_runResults;
  QStringList m_runFailures, m_runCancelled;
  QString m_runCacheError;
  quint64 m_runSession = 0; // 提交时的任务会话号；工程切换后结果不再写入属性
  int m_runIndex = -1;
  DataCatalog *m_catalog = nullptr; // 拥有方 = 组装根（DataImportService 实例）
  QString m_projectDir;
  bool m_busy = false, m_loading = false;
};
