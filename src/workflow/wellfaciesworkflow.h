// 层：功能
#pragma once
#include "ai/wellfaciesservice.h"
#include <QVariantList>
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
signals:
  void modelsChanged(const QVariantList &models);
  void availabilityChanged(bool ready, const QString &reason);
  void busyChanged(bool busy);
  void statusChanged(const QString &message);
  void resultReady(const WellFaciesResult &result);
  void resultCleared();

private:
  void updateInput(bool restoreCache = true);
  void loadKeyFromKeychain();
  int m_configGeneration = 0; // configure() 递增：迟到的钥匙串读结果不覆盖新配置
  QString cachePath() const;
  WellFaciesService m_service;
  WellFaciesConfig m_config;
  WellComposite::ComprehensiveWellData m_data;
  QVector<WellFaciesModel> m_models;
  QString m_modelId, m_modelError;
  WellFaciesInput m_input;
  bool m_busy = false, m_loading = false;
};
