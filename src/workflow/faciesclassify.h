// 层：功能
#pragma once
#include "domain/faciesclassification.h"
#include "services/faciestraining.h"
#include "services/paleotaskservice.h"
#include <QObject>
#include <QPointer>
#include <memory>
class DataCatalog;
class PaleoProjectStore;
class QgisLayerService;
namespace paleo::crossplot {
class FaciesClassifyWorkflow : public QObject {
  Q_OBJECT
public:
  FaciesClassifyWorkflow(PaleoTaskService *, PaleoProjectStore *,
                         QgisLayerService *, QObject *parent = nullptr);
  void setCatalog(DataCatalog *, const QString &projectDir);
  void clear();
  void setSamples(std::shared_ptr<const SampleSet>);
  std::shared_ptr<const SampleSet> samples() const { return m_samples; }
  const Classification &classification() const { return m_classification; }
  PaleoTask *classify(const ClassificationOptions &);
  FaciesProduct write(const QString &horizon);
  void cancel();
  // 训练集维护（自由词标注）：行号 → 类码，新类名追加 classNames，重复标注
  // 以后者覆盖。setSamples 换样本时训练集整体清空（防行错位）；标注变更会
  // 把训练集置 dirty——classify 监督分支遇 dirty 即要求重新训练，防止用
  // 陈旧模型静默推理。
  void assignTrainingLabel(const QVector<int> &rows, const QString &className);
  void clearTraining();
  const TrainingSet &trainingSet() const { return m_training; }
  QString trainingSummary() const;
public slots:
  // 训练监督模型：门禁（无样本 / validate 不拒）后任务化跑
  // FaciesTrainingService::train；成功发 trainingReady(report)。report 含
  // 混淆矩阵 cells 平铺、classNames、每类 precision/recall、folds、warnings。
  void train(const ClassificationOptions &);

signals:
  void classificationReady(const paleo::crossplot::Classification &);
  void failed(const QString &);
  void busyChanged(bool);
  void progressChanged(int);
  void productReady(const paleo::crossplot::FaciesProduct &);
  void trainingChanged();
  void trainingReady(const QVariantMap &report);

private:
  QVariantMap trainingReport(const TrainedModel &) const;
  QPointer<PaleoTaskService> m_tasks;
  QPointer<PaleoProjectStore> m_store;
  QPointer<QgisLayerService> m_layers;
  QPointer<DataCatalog> m_catalog;
  QString m_projectDir;
  std::shared_ptr<const SampleSet> m_samples;
  Classification m_classification;
  ClassificationOptions m_options;
  TrainingSet m_training;
  bool m_trainingDirty = false; // 标注自上次成功训练后变更过
  std::shared_ptr<const TrainedModel> m_trained;
  QPointer<PaleoTask> m_task;
  quint64 m_generation = 0;
};
} // namespace paleo::crossplot
