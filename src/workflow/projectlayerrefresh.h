// 层：功能
#pragma once
#include <QObject>
#include <QPointer>
#include <QByteArray>
#include <QStringList>
class DataCatalog;
struct ProjectLayerData {
  QByteArray wellsBytes, trajectoryBytes;
  QStringList warnings;
  bool wellsPrepared = false, trajectoriesPrepared = false;
};
Q_DECLARE_METATYPE(ProjectLayerData)

// Coalesce import notifications into one background data preparation. Only
// value results cross back to the owner; the app publishes and attaches layers.
class ProjectLayerRefreshWorkflow : public QObject {
  Q_OBJECT
public:
  explicit ProjectLayerRefreshWorkflow(QObject *parent = nullptr);
  void request(DataCatalog *catalog, const QString &directory);
  void cancel();
  static ProjectLayerData prepare(DataCatalog &snapshot, const QString &directory);
signals:
  void prepared(const QString &directory, quint64 mutationSeq, const ProjectLayerData &data);
private:
  QPointer<DataCatalog> m_catalog;
  QString m_directory;
  quint64 m_generation = 0;
  bool m_running = false, m_pending = false;
};
