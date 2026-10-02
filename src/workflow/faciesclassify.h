// 层：功能
#pragma once
#include "domain/faciesclassification.h"
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
signals:
  void classificationReady(const paleo::crossplot::Classification &);
  void failed(const QString &);
  void busyChanged(bool);
  void progressChanged(int);
  void productReady(const paleo::crossplot::FaciesProduct &);

private:
  QPointer<PaleoTaskService> m_tasks;
  QPointer<PaleoProjectStore> m_store;
  QPointer<QgisLayerService> m_layers;
  QPointer<DataCatalog> m_catalog;
  QString m_projectDir;
  std::shared_ptr<const SampleSet> m_samples;
  Classification m_classification;
  QPointer<PaleoTask> m_task;
  quint64 m_generation = 0;
};
} // namespace paleo::crossplot
