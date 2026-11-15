// 层：功能
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

class PaleoTask;
class PaleoTaskService;
namespace paleo::python { class PythonEnvironmentRegistry; }

// Host settings workflow. It is not registered as an Agent tool; all I/O and
// probes use the existing task service, while owner snapshots remain immediate.
class PythonEnvironmentController : public QObject {
  Q_OBJECT
public:
  PythonEnvironmentController(std::shared_ptr<paleo::python::PythonEnvironmentRegistry>, PaleoTaskService *, QObject *parent = nullptr);
  ~PythonEnvironmentController() override;
  bool busy() const { return m_busy; }
  QJsonObject redactedDiagnostics() const;
public slots:
  void reload();
  void install(const QString &package, const QString &anchor, const QString &store);
  void activate(const QJsonObject &form);
  void probe();
  void disable();
  void revoke(const QString &identityId);
  void rollback(const QString &identityId);
  void exportDiagnostics(const QString &path);
  void cancel();
signals:
  void configurationChanged(const QJsonObject &form, const QJsonArray &history);
  void diagnosticsChanged(const QJsonObject &projection);
  void installationCompleted(const QJsonObject &installed);
  void busyChanged(bool busy);
  void messageChanged(const QString &message);
private:
  enum class Kind { Reload, Install, Activate, Probe, Disable, Revoke, Rollback, Export };
  using Work = std::function<QString(PaleoTask *, QJsonObject &)>;
  void start(Kind, const QString &title, Work, bool replaceForm = false);
  void publishSnapshot(bool replaceForm);
  void complete(Kind, const QString &code, const QJsonObject &data, bool replaceForm);
  std::shared_ptr<paleo::python::PythonEnvironmentRegistry> m_registry;
  QPointer<PaleoTaskService> m_service;
  QPointer<PaleoTask> m_task;
  quint64 m_generation = 0, m_formRevision = 0;
  QString m_probeIdentity, m_probeRevision;
  QJsonObject m_trustedProbe, m_generatedProbe;
  QJsonObject m_formPolicy;
  bool m_busy = false, m_formLoaded = false, m_disabling = false;
};
