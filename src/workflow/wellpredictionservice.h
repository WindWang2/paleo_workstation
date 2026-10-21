// 层：功能
#pragma once
#include "../ai/remotepredictionservice.h"
#include "../ai/wellfaciesservice.h"
#include <QPointer>
#include <QMap>
#include <atomic>
#include <memory>
class DataCatalog;
class PaleoTaskService;

// 编图测井入口复用测井面板的 API 2.0 服务与输入计划；只产出冻结结果，
// 工区属性与图件统一由 MappingWorkbench 发布。
class WellPredictionService : public RemotePredictionService {
  Q_OBJECT
public:
  explicit WellPredictionService(QObject *parent = nullptr);
  ~WellPredictionService() override;
  void setTaskService(PaleoTaskService *tasks) { m_tasks = tasks; }
  void bindCatalog(DataCatalog *catalog, const QString &dir);
  QString statusHint() const;
  void start(const RemotePredictionRequest &request) override;
  // 预测专项 P0：批量逐井准备检查（干跑）。与 start() 走同一条
  // prepare（后台读曲线/分层/岩性）→ GET /models → plan
  // （planWellFaciesFormationRun）链路，规则与正式运行完全一致；只是不发
  // POST /predict、不产出结果。完成发 readinessReady。
  void check(const RemotePredictionRequest &request);
  void cancel() override;
signals:
  void detailChanged(const QString &requestId, const QString &detail);
  // wells：每口请求井一行 {id, name, ready, reason, groups}；error 非空 =
  // 整批无法检查（服务未配置/模型列表失败），此时 wells 为空。
  void readinessReady(const QString &requestId, const QVariantList &wells,
                      const QString &error);
private:
  PaleoTaskService *m_tasks = nullptr;
  void configure(const WellFaciesConfig &config);
  void prepare();
  void plan(const QVector<WellFaciesModel> &models);
  void next();
  void finish();
  void reportReadiness();
  void fail(const QString &reason);
  void skip(int well, const QString &reason);
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
  bool m_dryRun = false;
  quint64 m_generation = 0;
  std::shared_ptr<std::atomic_bool> m_cancelled;
};
