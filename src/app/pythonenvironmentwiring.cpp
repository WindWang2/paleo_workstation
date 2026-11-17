// 层：组装根
#include "pythonenvironmentwiring.h"
#include "pythonnumerictablewiring.h"
#include "pythonseismictracewiring.h"
#include "services/pythonenvironmentregistry.h"
#include "workflow/pythonenvironmentoperation.h"
#include "services/pythonanalysiscapabilityspec.h"
#include "services/pythongenerated.h"
#include "services/pythonnumerictable.h"
#include "services/pythontableoutputspec.h"
#include "workflow/pythontableoutputoperation.h"
#include "services/pythongeneratedoutputspec.h"
#include "workflow/pythongeneratedoutputoperation.h"
#include "catalog/datacatalog.h"
#include "workflow/documentsession.h"
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>

namespace {
using namespace paleo;
using namespace paleo::python;
EnvironmentSelection current(const std::shared_ptr<PythonEnvironmentRegistry> &registry) {
  auto selected = registry->active(EnvironmentMode::Generated);
  return selected.runtime ? selected : registry->active(EnvironmentMode::Trusted);
}
class EnvironmentBinding final : public QObject {
public:
  EnvironmentBinding(AiChatToolRunner *owner, std::shared_ptr<PythonEnvironmentRegistry> registry)
      : QObject(owner), m_owner(owner), m_registry(std::move(registry)), m_watcher(this) {
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { schedule(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
      auto parent = QFileInfo(m_registry->configurationPath()).absolutePath();
      while (!QFileInfo::exists(parent) && !QDir(parent).isRoot()) parent = QFileInfo(parent).absolutePath();
      if (QFileInfo::exists(m_registry->configurationPath()) || parent != m_watchedParent) schedule();
    });
    setObjectName("_paleoPythonEnvironmentBinding"); watch();
  }
  void adapters(DataCatalog *catalog, document::DocumentSession *document, capability::CapabilityHost *host) {
    if (!host || (m_host == host && m_catalog == catalog && m_document == document)) return;
    m_host = host; m_catalog = catalog; m_document = document;
    const auto selector = assemblePythonAnalysisProvider(catalog, document, host);
    for (const auto kind : {AnalysisKind::WellQuality, AnalysisKind::AttributeCorrelation, AnalysisKind::Generated})
      host->registerExecutionAdapter(kind == AnalysisKind::Generated ? QLatin1String(GeneratedCapability) : analysisCapabilityId(kind),
          environmentAnalysisAdapter(kind, m_registry, selector));
    host->registerExecutionAdapter(QLatin1String(NumericTableCapability),
        environmentNumericTableAdapter(m_registry,assemblePythonNumericTableProvider(catalog,document,host)));
    host->registerExecutionAdapter(QLatin1String(SeismicTraceCapability),
        environmentSeismicTraceAdapter(m_registry,assemblePythonSeismicTraceProvider(catalog,document,host)));
    host->registerExecutionAdapter(QLatin1String(TableOutputsCapability),
        environmentTableOutputsAdapter(m_registry,assemblePythonNumericTableProvider(catalog,document,host)));
    host->registerExecutionAdapter(QLatin1String(GeneratedOutputsCapability),environmentGeneratedOutputsAdapter(m_registry,
        assemblePythonNumericTableProvider(catalog,document,host),assemblePythonSeismicTraceProvider(catalog,document,host)));
  }
  void refresh() {
    if (!m_owner || !m_owner->taskService()) return;
    const auto revision = m_registry->beginRefresh(); m_generation = revision;
    if (m_task) m_task->requestInterruption();
    const auto registry = m_registry; const auto legacy = captureLegacyPythonEnvironment();
    const QPointer<EnvironmentBinding> guard(this);
    auto *created = m_owner->taskService()->start(QObject::tr("Python 环境状态"), [registry, legacy, revision](PaleoTask *task) {
      const auto code = registry->refresh(legacy, revision, [task] { return task->cancelRequested(); }, false);
      return code == "cancelled" ? code : QString();
    }, QString(), true);
    if (!guard) { if (created) created->requestInterruption(); return; }
    if (m_generation != revision) { if (created) created->requestInterruption(); return; }
    m_task = created;
    if (!created) return;
    connect(created, &PaleoTask::finished, this, [guard, revision] {
      if (!guard || guard->m_generation != revision) return;
      guard->watch(); guard->prewarm(EnvironmentMode::Trusted, revision);
      if (!guard || guard->m_generation != revision) return;
      guard->prewarm(EnvironmentMode::Generated, revision);
    }, Qt::SingleShotConnection);
  }
private:
  void prewarm(EnvironmentMode mode, quint64 generation) {
    if (!m_owner || !m_owner->taskService()) return;
    const auto selection = m_registry->active(mode); if (!selection.ok()) return;
    if (mode == EnvironmentMode::Trusted && selection.trusted->snapshot().value("executionReady").toBool()) return;
    if (mode == EnvironmentMode::Generated && selection.sandbox->snapshot().value("ready").toBool()) return;
    const auto registry = m_registry; const auto title = mode == EnvironmentMode::Generated ? QObject::tr("Python 隔离状态") : QObject::tr("Python 环境状态");
    const QPointer<EnvironmentBinding> guard(this);
    auto *task = m_owner->taskService()->start(title, [registry, id = selection.identity.id, mode, token = selection.interrupted,
        global = selection.globalInterruption](PaleoTask *task) {
      registry->probe(id, mode, [task, token, global] { return task->cancelRequested() || token->load() || global->load(); }); return QString();
    }, QString(), true);
    if ((!guard || m_generation != generation) && task) task->requestInterruption();
  }
  void schedule() {
    if (m_scheduled) return;
    m_scheduled = true;
    QTimer::singleShot(0, this, [this] { m_scheduled = false; watch(); refresh(); });
  }
  void watch() {
    const auto path = m_registry->configurationPath();
    auto parent = QFileInfo(path).absolutePath();
    while (!QFileInfo::exists(parent) && !QDir(parent).isRoot()) parent = QFileInfo(parent).absolutePath();
    const auto oldFiles = m_watcher.files(), oldDirectories = m_watcher.directories();
    if (!oldFiles.isEmpty()) m_watcher.removePaths(oldFiles);
    if (!oldDirectories.isEmpty()) m_watcher.removePaths(oldDirectories);
    if (QFileInfo::exists(parent)) m_watcher.addPath(parent);
    m_watchedParent = parent;
    if (QFileInfo::exists(path)) m_watcher.addPath(path);
  }
  QPointer<AiChatToolRunner> m_owner;
  std::shared_ptr<PythonEnvironmentRegistry> m_registry;
  QFileSystemWatcher m_watcher;
  QPointer<PaleoTask> m_task;
  QPointer<capability::CapabilityHost> m_host;
  QPointer<DataCatalog> m_catalog;
  QPointer<document::DocumentSession> m_document;
  quint64 m_generation = 0;
  bool m_scheduled = false;
  QString m_watchedParent;
};
QString readiness(const std::shared_ptr<PythonEnvironmentRegistry> &registry, const QString &name,
                  const QJsonObject &arguments, bool listing) {
  const bool generated = name == QLatin1String(GeneratedTool) || name == QLatin1String(NumericTableTool) ||
      name == QLatin1String(SeismicTraceTool) || name == QLatin1String(TableOutputsTool) || name == QLatin1String(GeneratedOutputsTool);
  if (!generated && name != QLatin1String(WellQualityTool) && name != QLatin1String(CorrelationTool)) return {};
  const auto selection = registry->active(generated ? EnvironmentMode::Generated : EnvironmentMode::Trusted);
  // Publication uses a frozen historical identity. The stable Host adapter
  // authenticates that record; availability of the current runtime is irrelevant.
  if (!listing && arguments.value("publish_intent") == "publish") {
    const auto state = registry->status();
    return state.value(generated ? "allowGenerated" : "allowTrusted").toBool() ? QString() : QStringLiteral("python_execution_denied");
  }
  if (!selection.ok()) return selection.code;
  const auto runtime = selection.runtime->snapshot(); if (!runtime.ready()) return runtime.code;
  if (generated) {
    const auto state = selection.sandbox->snapshot(); return state.value("ready").toBool() ? QString() : state.value("code").toString("python_sandbox_unavailable");
  }
  const auto state = selection.trusted->snapshot();
  return state.value("executionReady").toBool() ? QString() : state.value("code").toString("python_trusted_execution_unprobed");
}
} // namespace

bool assemblePythonEnvironmentRegistry(AiChatToolRunner *runner, AiChatToolContext &context, DataCatalog *catalog,
    paleo::document::DocumentSession *document, paleo::capability::CapabilityHost *host, const std::function<bool()> &bindingIsCurrent) {
  const QPointer<AiChatToolRunner> owner(runner);
  const auto valid = [&] { return owner && bindingIsCurrent && bindingIsCurrent(); };
  if (!valid()) return false;
  auto registry = runner->pythonEnvironments();
  if (!registry) { registry = std::make_shared<PythonEnvironmentRegistry>(); runner->setPythonEnvironments(registry); }
  auto *binding = dynamic_cast<EnvironmentBinding *>(runner->findChild<QObject *>("_paleoPythonEnvironmentBinding", Qt::FindDirectChildrenOnly));
  if (!binding) binding = new EnvironmentBinding(runner, registry);
  const auto selected = current(registry);
  context.pythonRuntime = selected.runtime ? selected.runtime : std::make_shared<RuntimeProvider>(RuntimeConfig{});
  context.pythonTrusted = selected.trusted ? selected.trusted : std::make_shared<TrustedReadinessProvider>(context.pythonRuntime);
  context.pythonSandbox = selected.sandbox ? selected.sandbox : std::make_shared<SandboxProvider>(context.pythonRuntime);
  context.runtimeReadiness = [registry](const auto &name, const auto &arguments, bool listing) { return readiness(registry, name, arguments, listing); };
  binding->adapters(catalog, document, host); if (!valid()) return false;
  binding->refresh(); return valid();
}
