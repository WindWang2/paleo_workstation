// 层：功能
#pragma once
#include "../services/derivationgraph.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
class DataCatalog;
class PaleoTaskService;
namespace paleo::jobs {
template <class T> class JobRunner;
}
class DerivationWorkflow : public QObject {
  Q_OBJECT
public:
  explicit DerivationWorkflow(QObject *parent = nullptr);
  ~DerivationWorkflow() override;
  void request(DataCatalog *catalog, PaleoTaskService *tasks,
               const paleo::derivation::Query &query);
  void cancel();
signals:
  void ready(const paleo::derivation::Graph &graph);

private:
  struct Job;
  void launch();
  QPointer<DataCatalog> m_catalog;
  QPointer<PaleoTaskService> m_tasks;
  PaleoTaskService *m_fallback;
  paleo::jobs::JobRunner<Job> *m_runner;
  paleo::derivation::Query m_query;
  QTimer m_timer;
  quint64 m_generation = 0;
  bool m_pending = false;
  QString m_requestedPath;
  quint64 m_requestedSequence = 0;
};
