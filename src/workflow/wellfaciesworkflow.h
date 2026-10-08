// 层：功能
#pragma once
#include "ai/wellfaciesservice.h"
#include <QVariantList>
class DataCatalog;
class QgisLayerService;
class QgsVectorLayer;
class WellFaciesWorkflow : public QObject {
  Q_OBJECT
public:
  explicit WellFaciesWorkflow(QObject *parent = nullptr);
  WellFaciesConfig config() const { return m_config; }
  void configure(const WellFaciesConfig &config, bool persist = true);
  void setData(const WellComposite::ComprehensiveWellData &data);
  void selectModel(const QString &id);
  void refreshModels();
  void run();
  void cancel();
  // 绑定图层服务时预测回写可维护矢量相属性；未绑定保留旧资产兼容缝。
  // 方向 69：旧接口落 catalog DERIVED 解释岩性资产（well_litho_intervals
  // schema 1）+ per-well interpretation 链接。projectDir 空 = 按 catalog
  // 当前工程解析（换工程自适应）。未绑定/不可写 → completed 仍调
  // publishLithoAsset，失败原因如实进状态行（不静默跳过）。
  void setCatalog(DataCatalog *catalog, const QString &projectDir = QString());
  void setLayerService(QgisLayerService *layers);
  void requestAttributeTable(bool factors = false);
  void refreshAttributes();
  // 预测结果 → DERIVED 资产（与 completed 信号同一条生产者路；测试直达缝）。
  // 成功返回空串；未登记返回如实原因（无 catalog/井名未解析/无有效段/
  // 登记失败）——失败不写资产（诚实面）。
  QString publishLithoAsset(const WellFaciesResult &result);
signals:
  void modelsChanged(const QVariantList &models);
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
  void updateInput(bool restoreCache = true);
  void loadKeyFromKeychain();
  int m_configGeneration = 0; // configure() 递增：迟到的钥匙串读结果不覆盖新配置
  QString cachePath() const;
  QString projectDir() const;
  QString publishAttributes(const WellFaciesResult &result);
  WellFaciesService m_service;
  WellFaciesConfig m_config;
  WellComposite::ComprehensiveWellData m_data, m_sourceData;
  QPointer<QgisLayerService> m_layers;
  QPointer<QgsVectorLayer> m_attributeLayer;
  QVector<WellFaciesModel> m_models;
  QString m_modelId, m_modelError;
  WellFaciesInput m_input;
  DataCatalog *m_catalog = nullptr; // 拥有方 = 组装根（DataImportService 实例）
  QString m_projectDir;
  bool m_busy = false, m_loading = false;
};
