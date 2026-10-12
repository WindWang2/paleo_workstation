// 层：功能
#pragma once
#include "../services/datagraph.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
class DataCatalog;
class PaleoTaskService;
class PaleoTask;
namespace paleo::jobs {
class JobRunnerBase;
template <class T> class JobRunner;
} // namespace paleo::jobs
class DataGraphWorkflow : public QObject {
  Q_OBJECT
public:
  explicit DataGraphWorkflow(QObject *parent = nullptr);
  ~DataGraphWorkflow() override;
  void setContext(DataCatalog *catalog, PaleoTaskService *tasks);
  void request(const paleo::datagraph::Query &query);
  void setPresentation(const paleo::datagraph::Presentation &presentation);
  void cancel();
  void exportGraph(const paleo::datagraph::Graph &graph, const QString &path);
signals:
  void ready(const paleo::datagraph::Graph &graph);
  void statusChanged(const QString &text, bool busy);
  void exportFinished(const QString &path, const QString &error);

private:
  struct Job;
  void launch();
  QPointer<DataCatalog> m_catalog;
  QPointer<PaleoTaskService> m_tasks;
  PaleoTaskService *m_fallback;
  paleo::jobs::JobRunner<Job> *m_runner;
  QTimer m_timer;
  paleo::datagraph::Query m_query;
  paleo::datagraph::Presentation m_presentation;
  std::shared_ptr<const paleo::datagraph::Index> m_index;
  QString m_cachePath;
  QVector<QPair<paleo::datagraph::Query, paleo::datagraph::Graph>> m_graphCache;
  quint64 m_cacheSequence = 0, m_generation = 0;
  bool m_pending = false;
  QVector<QPointer<PaleoTask>> m_exports;
};
