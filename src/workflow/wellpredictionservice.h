// 层：功能
#pragma once
#include "../ai/remotepredictionservice.h"
#include "../ai/wellfaciesservice.h"
#include <QPointer>
#include <QMap>
#include <atomic>
#include <memory>
class DataCatalog;

// 编图测井入口复用测井面板的 API 2.0 服务与输入计划；只产出冻结结果，
// 工区属性与图件统一由 MappingWorkbench 发布。
class WellPredictionService : public RemotePredictionService {
  Q_OBJECT
public:
  explicit WellPredictionService(QObject *parent = nullptr);
  ~WellPredictionService() override;
  void bindCatalog(DataCatalog *catalog, const QString &dir);
  QString statusHint() const;
  void start(const RemotePredictionRequest &request) override;
  void cancel() override;
signals:
  void detailChanged(const QString &requestId, const QString &detail);
private:
  void configure(const WellFaciesConfig &config);
  void prepare();
  void plan(const QVector<WellFaciesModel> &models);
  void next();
  void finish();
  void fail(const QString &reason);
  void skip(int well, const QString &reason);
  int codeFor(const QString &label);
  WellFaciesService m_service;
  WellFaciesConfig m_config;
  bool m_configured = false;
  QPointer<DataCatalog> m_catalog;
  QString m_dir;
  RemotePredictionRequest m_request;
  QVector<WellComposite::ComprehensiveWellData> m_data;
  struct Run { int well; WellFaciesRun prediction; };
  QVector<Run> m_runs;
  QVector<QVector<WellFaciesResult>> m_results;
  QVariantList m_schema;
  QMap<int, QString> m_skipped;
  int m_index = -1;
  quint64 m_generation = 0;
  std::shared_ptr<std::atomic_bool> m_cancelled;
};
