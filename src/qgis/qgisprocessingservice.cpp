// 层：QGIS 封装
#include "qgisprocessingservice.h"
#include "qgiserrors_internal.h"

#include "../algorithms/paleoalgorithms.h"
#include "../metadata/paleoprojectstore.h"

#include <QCloseEvent>
#include <QDir>
#include <QLabel>
#include <QMainWindow>
#include <QTemporaryDir>
#include <QWidget>

#include <atomic>
#include <memory>

#include <qgsexception.h>
#include <qgsapplication.h>
#include <qgsgui.h>
#include <qgsmessagebar.h>
#include <qgspanelwidget.h>
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingalgorithmwidgetbase.h>
#include <qgsprocessingalgrunnertask.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsprocessingguiregistry.h>
#include <qgsprocessingparameters.h>
#include <qgsprocessingparameterswidget.h>
#include <qgsprocessingregistry.h>
#include <qgsprocessingwidgetwrapper.h>
#include <qgsproject.h>

namespace
{
using paleo::qgis_detail::setError;

  // §41.2 temp-then-merge: outputs a caller did not pin to a destination are
  // remapped into a process-wide temp pool (auto-removed at exit). Each run
  // gets its own subdirectory so concurrent / repeated runs never collide.
  // The merge step (PaleoProjectStore write queue) consumes the temp files.
  QString nextRunDir()
  {
    static QTemporaryDir s_tempPool;
    static std::atomic<unsigned long long> s_runCounter{0};
    const QString dir = s_tempPool.filePath(
      QStringLiteral("run%1").arg(s_runCounter.fetch_add(1)));
    QDir().mkpath(dir);
    return dir;
  }

  // -------------------------------------------------------------------------
  // Native algorithm dialog (QGIS 4.2 API surface)
  //
  // QGIS 4.2 replaced the 3.x QgsProcessingAlgorithmDialogBase (a QDialog) with
  // QgsProcessingAlgorithmWidgetBase — a QWidget hosted inside a top-level
  // dialog or dock by QgsDockableWidgetHelper — and moved the concrete
  // single-run implementation to the Python processing plugin
  // (python/plugins/processing/gui/{algorithm_widget.py,ParametersPanel.py}).
  // The two classes below are a minimal C++ port of that Python implementation:
  //
  //  * PaleoAlgorithmParametersPanel ≙ ParametersPanel: one
  //    QgsAbstractProcessingParameterWidgetWrapper per parameter, created
  //    through QgsGui::processingGuiRegistry(); implements the
  //    QgsProcessingParametersGenerator / QgsProcessingContextGenerator
  //    interfaces the wrappers need.
  //  * PaleoAlgorithmWidget ≙ AlgorithmWidget: the
  //    QgsProcessingAlgorithmWidgetBase subclass that wires Run →
  //    QgsProcessingAlgRunnerTask on the application's QgsTaskManager.
  //
  // Neither class declares new signals/slots, so no moc pass is required.
  // -------------------------------------------------------------------------

  class PaleoAlgorithmParametersPanel : public QgsProcessingParametersWidget,
                                      public QgsProcessingContextGenerator
  {
    public:
      PaleoAlgorithmParametersPanel(const QgsProcessingAlgorithm *algorithm,
                                    QgsProcessingAlgorithmWidgetBase *dialog,
                                    QgsProcessingContext *context)
        : QgsProcessingParametersWidget(algorithm, dialog)
        , m_dialog(dialog)
        , m_context(context)
      {
        initWidgets();
      }

      QgsProcessingContext *processingContext() override { return m_context; }

      // Collects the current widget values into an algorithm parameter map,
      // mirroring ParametersPanel.createProcessingParameters(). Validation
      // failures are reported on the host widget's message bar and exposed via
      // lastValidationError() (the returned map is empty in that case).
      QVariantMap createProcessingParameters(
          QgsProcessingParametersGenerator::Flags flags =
              QgsProcessingParametersGenerator::Flags()) override
      {
        const bool includeDefault =
            !flags.testFlag(QgsProcessingParametersGenerator::Flag::SkipDefaultValueParameters);
        const bool validate =
            !flags.testFlag(QgsProcessingParametersGenerator::Flag::SkipValidation);

        QVariantMap parameters = m_extraParameters;
        m_validationError.clear();

        const QgsProcessingParameterDefinitions defs = algorithm()->parameterDefinitions();
        for (const QgsProcessingParameterDefinition *param : defs)
        {
          if (param->flags().testFlag(Qgis::ProcessingParameterFlag::Hidden))
            continue;
          QgsAbstractProcessingParameterWidgetWrapper *wrapper =
              m_wrappers.value(param->name());
          if (!wrapper)
            continue;

          if (!param->isDestination())
          {
            // A missing widget means the parameter cannot be configured in this
            // panel (same skip rule as upstream ParametersPanel).
            if (!qobject_cast<QgsProcessingHiddenWidgetWrapper *>(wrapper) &&
                !wrapper->wrappedWidget())
              continue;

            const QVariant value = wrapper->parameterValue();
            if (param->defaultValue() != value || includeDefault)
              parameters.insert(param->name(), value);

            if (validate && !param->checkValueIsAcceptable(value, m_context) &&
                m_validationError.isEmpty())
            {
              m_validationError = tr("Wrong or missing parameter value: %1")
                                      .arg(param->description());
            }
          }
          else
          {
            QVariant value = wrapper->parameterValue();
            if (value.isValid() &&
                value.userType() == QMetaType::fromType<QgsProcessingOutputLayerDefinition>().id())
            {
              auto def = value.value<QgsProcessingOutputLayerDefinition>();
              def.destinationProject =
                  wrapper->customProperties()
                          .value(QStringLiteral("OPEN_AFTER_RUNNING"))
                          .toBool()
                      ? (m_context && m_context->project() ? m_context->project()
                                                        : QgsProject::instance())
                      : nullptr;
              value = QVariant::fromValue(def);
            }
            if (value.isValid() && (param->defaultValue() != value || includeDefault))
            {
              parameters.insert(param->name(), value);
              if (validate && m_context)
              {
                QString outError;
                const auto *destParam =
                    static_cast<const QgsProcessingDestinationParameter *>(param);
                if (!destParam->isSupportedOutputValue(value, *m_context, outError) &&
                    m_validationError.isEmpty())
                  m_validationError = outError;
              }
            }
          }
        }

        if (!m_validationError.isEmpty() && m_dialog && m_dialog->messageBar())
        {
          m_dialog->messageBar()->pushMessage(QString(), m_validationError,
                                              Qgis::MessageLevel::Warning);
        }
        // preprocessParameters is non-const (upstream calls it on the dialog's
        // owned algorithm instance, which is mutable); our panel sees the same
        // instance through a const pointer.
        return const_cast<QgsProcessingAlgorithm *>(algorithm())
            ->preprocessParameters(parameters);
      }

      // Applies preset values to the generated widgets; values for hidden
      // parameters are carried verbatim into createProcessingParameters().
      void setParameterValues(const QVariantMap &values)
      {
        m_extraParameters.clear();
        const QgsProcessingParameterDefinitions defs = algorithm()->parameterDefinitions();
        for (const QgsProcessingParameterDefinition *param : defs)
        {
          const QString name = param->name();
          if (!values.contains(name))
            continue;
          if (param->flags().testFlag(Qgis::ProcessingParameterFlag::Hidden))
          {
            m_extraParameters.insert(name, values.value(name));
            continue;
          }
          if (QgsAbstractProcessingParameterWidgetWrapper *wrapper = m_wrappers.value(name))
            wrapper->setParameterValue(values.value(name), *m_context);
        }
      }

      QString lastValidationError() const { return m_validationError; }

    protected:
      void initWidgets() override
      {
        // Base implementation only unhides the advanced-parameters group box;
        // all wrapper construction below follows ParametersPanel.initWidgets.
        QgsProcessingParametersWidget::initWidgets();

        QgsProcessingParameterWidgetContext widgetContext;
        QgsProject *widgetProject = (m_context && m_context->project())
                                        ? m_context->project()
                                        : QgsProject::instance();
        widgetContext.setProject(widgetProject);
        widgetContext.registerProcessingContextGenerator(this);
        if (m_dialog && m_dialog->messageBar())
          widgetContext.setMessageBar(m_dialog->messageBar());

        QList<QgsAbstractProcessingParameterWidgetWrapper *> wrappers;

        const QgsProcessingParameterDefinitions defs = algorithm()->parameterDefinitions();
        for (const QgsProcessingParameterDefinition *param : defs)
        {
          if (param->flags().testFlag(Qgis::ProcessingParameterFlag::Hidden) ||
              param->isDestination())
            continue;

          std::unique_ptr<QgsAbstractProcessingParameterWidgetWrapper> wrapper(
              QgsGui::processingGuiRegistry()->createParameterWidgetWrapper(
                  param, Qgis::ProcessingMode::Standard));
          if (!wrapper)
          {
            // Parameter type with no registered GUI factory — keep a hidden
            // value holder so preset/programmatic values still flow through.
            wrapper = std::make_unique<QgsProcessingHiddenWidgetWrapper>(
                param, Qgis::ProcessingMode::Standard, this);
          }

          wrapper->setDialog(m_dialog);
          wrapper->setWidgetContext(widgetContext);
          wrapper->registerProcessingContextGenerator(this);
          wrapper->registerProcessingParametersGenerator(this);

          if (QWidget *widget = wrapper->createWrappedWidget(*m_context))
          {
            if (QLabel *label = wrapper->createWrappedLabel())
              addParameterLabel(param, label);
            addParameterWidget(param, widget, wrapper->stretch());
          }

          wrapper->setParent(this);
          m_wrappers.insert(param->name(), wrapper.get());
          wrappers.append(wrapper.release());
        }

        for (const QgsProcessingParameterDefinition *output :
             algorithm()->destinationParameterDefinitions())
        {
          if (output->flags().testFlag(Qgis::ProcessingParameterFlag::Hidden))
            continue;

          std::unique_ptr<QgsAbstractProcessingParameterWidgetWrapper> wrapper(
              QgsGui::processingGuiRegistry()->createParameterWidgetWrapper(
                  output, Qgis::ProcessingMode::Standard));
          if (!wrapper)
          {
            wrapper = std::make_unique<QgsProcessingHiddenWidgetWrapper>(
                output, Qgis::ProcessingMode::Standard, this);
          }

          wrapper->setDialog(m_dialog);
          wrapper->setWidgetContext(widgetContext);
          wrapper->registerProcessingContextGenerator(this);
          wrapper->registerProcessingParametersGenerator(this);

          if (QLabel *label = wrapper->createWrappedLabel())
            addOutputLabel(label);
          if (QWidget *widget = wrapper->createWrappedWidget(*m_context))
            addOutputWidget(widget, wrapper->stretch());

          wrapper->setParent(this);
          m_wrappers.insert(output->name(), wrapper.get());
          wrappers.append(wrapper.release());
        }

        for (QgsAbstractProcessingParameterWidgetWrapper *wrapper : std::as_const(wrappers))
          wrapper->postInitialize(wrappers);
      }

    private:
      QgsProcessingAlgorithmWidgetBase *m_dialog = nullptr;
      QgsProcessingContext *m_context = nullptr; // owned by the host widget
      QHash<QString, QgsAbstractProcessingParameterWidgetWrapper *> m_wrappers;
      QVariantMap m_extraParameters;
      QString m_validationError;
  };

  class PaleoAlgorithmWidget : public QgsProcessingAlgorithmWidgetBase
  {
    public:
      // Takes ownership of \a algorithm. \a parentWindow may be nullptr — with
      // WidgetFlag::NoDocking the widget is always hosted in a standalone
      // top-level dialog created by QgsDockableWidgetHelper.
      explicit PaleoAlgorithmWidget(QgsProcessingAlgorithm *algorithm,
                                    QMainWindow *parentWindow,
                                    QgsProject *project)
        : QgsProcessingAlgorithmWidgetBase(
              parentWindow, QgsProcessingAlgorithmWidgetBase::WidgetMode::Single,
              QgsProcessingAlgorithmWidgetBase::WidgetFlag::NoDocking,
              Qgis::DockableWidgetInitialState::ForceDialog)
      {
        m_context = std::make_shared<QgsProcessingContext>();
        QgsProject *resolved = project ? project : QgsProject::instance();
        if (resolved)
        {
          m_context->setProject(resolved);
          m_context->setTransformContext(resolved->transformContext());
        }
        setAlgorithm(algorithm);
        auto *panel = new PaleoAlgorithmParametersPanel(algorithm, this, m_context.get());
        setMainWidget(panel);

        if (QWidget *top = window())
          top->installEventFilter(this);
      }

      ~PaleoAlgorithmWidget() override
      {
        if (m_running && m_currentTask)
        {
          m_currentTask->cancel();
          if (!m_currentTask->waitForFinished(3000))
          {
            // Timeout elapsed while worker still running: explicitly disconnect all
            // signal connections to this widget to prevent post-destruction callbacks.
            m_currentTask->disconnect(this);
          }
        }
        if (m_currentTask)
        {
          m_currentTask->disconnect(this);
          m_currentTask = nullptr;
        }
        m_running = false;
        disconnect(this);
        // Worker pointer safety: context and feedback lifetimes are bound to the
        // runner task's destroyed signal via TaskPayload, so even if background
        // execution outlives this widget, no dangling pointer or use-after-free
        // occurs. Do NOT call delete m_feedback.
      }

      void showEvent(QShowEvent *e) override
      {
        QgsProcessingAlgorithmWidgetBase::showEvent(e);
        if (QWidget *top = window())
          top->installEventFilter(this);
      }

      bool eventFilter(QObject *watched, QEvent *e) override
      {
        if (watched == window() && e->type() == QEvent::Close)
        {
          if (m_running && m_currentTask)
          {
            m_currentTask->cancel();
            if (!m_currentTask->waitForFinished(500))
            {
              m_closeRequested = true;
              e->ignore();
              setInfo(tr("Algorithm cancellation requested. Waiting for background worker to terminate..."), false);
              return true;
            }
          }
        }
        return QgsProcessingAlgorithmWidgetBase::eventFilter(watched, e);
      }

      void closeEvent(QCloseEvent *e) override
      {
        if (m_running && m_currentTask)
        {
          m_currentTask->cancel();
          if (!m_currentTask->waitForFinished(500))
          {
            m_closeRequested = true;
            e->ignore();
            setInfo(tr("Algorithm cancellation requested. Waiting for background worker to terminate..."), false);
            return;
          }
        }
        QgsProcessingAlgorithmWidgetBase::closeEvent(e);
      }

      QVariantMap createProcessingParameters(
          QgsProcessingParametersGenerator::Flags flags =
              QgsProcessingParametersGenerator::Flags()) override
      {
        if (auto *panel = parametersPanel())
          return panel->createProcessingParameters(flags);
        return QVariantMap();
      }

      QgsProcessingContext *processingContext() override { return m_context.get(); }

      void setParameters(const QVariantMap &values) override
      {
        if (auto *panel = parametersPanel())
          panel->setParameterValues(values);
      }

      bool isRunning() override { return m_running; }

      bool isFinalized() override { return !m_running; }

    protected:
      // Run button → collect + validate parameters, then hand the run to the
      // application task manager (same path as the native Processing UI).
      // (runAlgorithm/finished are slots on the base — overrides dispatch
      // virtually, no moc needed for this subclass.)
      void runAlgorithm() override
      {
        if (!algorithm() || m_running)
          return;

        const QVariantMap params = createProcessingParameters();
        if (auto *panel = parametersPanel(); panel && !panel->lastValidationError().isEmpty())
          return; // already reported on the message bar by the panel

        QString message;
        if (!algorithm()->checkParameterValues(params, *m_context, &message))
        {
          // Message bar instead of QMessageBox: static QMessageBox helpers exec
          // a nested event loop, which hangs offscreen test runs.
          if (messageBar())
            messageBar()->pushMessage(tr("Unable to execute algorithm"), message,
                                      Qgis::MessageLevel::Warning);
          return;
        }

        m_feedback = std::shared_ptr<QgsProcessingFeedback>(createFeedback());

        applyContextOverrides(m_context.get());
        blockControlsWhileRunning();
        setExecutedAnyResult(true);
        cancelButton()->setEnabled(
            algorithm()->flags().testFlag(Qgis::ProcessingAlgorithmFlag::CanCancel));
        showLog();

        m_feedback->pushVersionInfo(algorithm()->provider());
        if (algorithm()->provider() && !algorithm()->provider()->warningMessage().isEmpty())
          m_feedback->reportError(algorithm()->provider()->warningMessage());
        m_feedback->pushInfo(tr("Algorithm started"));
        setInfo(tr("<b>Algorithm '%1' starting&hellip;</b>")
                    .arg(algorithm()->displayName()),
                false, false);

        m_running = true;
        auto *task = new QgsProcessingAlgRunnerTask(algorithm(), params, *m_context, m_feedback.get());
        m_currentTask = task;

        auto payload = std::make_shared<TaskPayload>();
        payload->context = m_context;
        payload->feedback = m_feedback;

        connect(task, &QObject::destroyed, task, [payload]() mutable {
          payload.reset();
        });

        connect(task, &QgsProcessingAlgRunnerTask::executed, this,
                [this](bool successful, const QVariantMap &results) {
                  m_running = false;
                  m_currentTask = nullptr;
                  finished(successful, results, *m_context, m_feedback.get());
                  if (m_closeRequested)
                  {
                    m_closeRequested = false;
                    if (window())
                      window()->close();
                    else
                      close();
                  }
                });
        // If prepare() failed inside the task ctor the task self-cancels; the
        // task manager still runs its finished() → executed(false) path, so the
        // cleanup lambda above always fires. (QgsTask::isCanceled is protected
        // in C++, so we rely on the signal rather than the pre-check upstream
        // Python performs.)
        setCurrentTask(task); // base connects algExecuted + submits to task manager
      }

      void finished(bool successful, const QVariantMap &result,
                    QgsProcessingContext &context,
                    QgsProcessingFeedback *feedback) override
      {
        Q_UNUSED(context);
        Q_UNUSED(feedback);
        m_currentTask = nullptr;
        setExecuted(successful);
        setResults(result);
        if (algorithm())
        {
          setInfo(tr("Algorithm '%1' finished").arg(algorithm()->displayName()),
                  false);
        }
        emit algorithmFinished(successful, result);
        resetGui();
      }

    private:
      PaleoAlgorithmParametersPanel *parametersPanel()
      {
        return static_cast<PaleoAlgorithmParametersPanel *>(mainWidget());
      }

      struct TaskPayload
      {
        std::shared_ptr<QgsProcessingContext> context;
        std::shared_ptr<QgsProcessingFeedback> feedback;
      };

      std::shared_ptr<QgsProcessingContext> m_context;
      std::shared_ptr<QgsProcessingFeedback> m_feedback;
      bool m_running = false;
      bool m_closeRequested = false;
      QPointer<QgsProcessingAlgRunnerTask> m_currentTask;
  };

} // namespace

QgisProcessingService::QgisProcessingService(PaleoProjectStore *store, QObject *parent)
  : QObject(parent)
  , m_store(store)
{
  // Paleo algorithms enter the registry only via PaleoProvider.
  // NOTE: algorithms/paleoalgorithms.cpp is another workstream's file — until
  // it is linked, targets that need this service shim PaleoProvider::
  // loadAlgorithms weakly (see tst_services2.cpp). After integration the real
  // implementation wins and loadAlgorithms populates the provider.
  QgsProcessingRegistry *reg = QgsApplication::processingRegistry();
  if (reg && !reg->providerById(QStringLiteral("paleo")))
    reg->addProvider(new PaleoProvider());
}

void QgisProcessingService::setProject(QgsProject *project)
{
  m_project = project;
}

QWidget *QgisProcessingService::lastAlgorithmDialog() const
{
  return m_lastDialog;
}

namespace
{
class HookFeedback : public QgsProcessingFeedback
{
public:
  explicit HookFeedback(const QgisProcessingService::ProcessingHooks &hooks)
    : m_hooks(hooks)
  {
    // QgsFeedback::setProgress is not virtual. progressChanged fires on the
    // same thread, after the 0.1% bucket changes, before the caller checks
    // isCanceled().
    connect(this, &QgsFeedback::progressChanged, this, [this](double progress) {
      if (m_hooks.cancelled && m_hooks.cancelled())
        cancel();
      if (m_hooks.progress)
        m_hooks.progress(progress);
    });
  }

private:
  QgisProcessingService::ProcessingHooks m_hooks;
};
} // namespace

QVariantMap QgisProcessingService::run(const QString &algorithmId, const QVariantMap &parameters, QString *error,
                                       const ProcessingHooks &hooks)
{
  QgsProcessingRegistry *reg = QgsApplication::processingRegistry();
  const QgsProcessingAlgorithm *alg = reg ? reg->algorithmById(algorithmId) : nullptr;
  if (!alg)
  {
    setError(error, tr("no processing algorithm registered as '%1'").arg(algorithmId));
    return QVariantMap();
  }

  // create() (not createInstance()): it additionally runs initAlgorithm(),
  // which is what installs the parameter definitions we inspect below.
  std::unique_ptr<QgsProcessingAlgorithm> instance(alg->create());
  if (!instance)
  {
    setError(error, tr("algorithm '%1' could not create an instance").arg(algorithmId));
    return QVariantMap();
  }

  // Temp-then-merge contract: remap every destination parameter the caller did
  // not specify into this run's temp dir. Callers merge outputs into
  // project.gpkg through PaleoProjectStore::enqueueWrite afterwards — task
  // code never writes into the gpkg directly.
  QVariantMap params = parameters;
  const QString runDir = nextRunDir();
  const QgsProcessingParameterDefinitions destDefs = instance->destinationParameterDefinitions();
  for (const QgsProcessingParameterDefinition *def : destDefs)
  {
    if (params.contains(def->name()))
      continue; // caller-specified destination wins

    const auto *destDef = static_cast<const QgsProcessingDestinationParameter *>(def);
    const QString ext = destDef->defaultFileExtension();
    QString path = QDir(runDir).filePath(
      ext.isEmpty() ? def->name() : def->name() + QStringLiteral(".") + ext);
    if (ext.isEmpty())
      QDir().mkpath(path); // folder destination: create, don't name a file
    params.insert(def->name(), path);
  }

  QgsProcessingContext context;
  HookFeedback feedback(hooks);
  if (hooks.cancelled && hooks.cancelled())
  {
    setError(error, tr("已取消"));
    return QVariantMap();
  }
  bool ok = false;
  QVariantMap results;
  try
  {
    // Synchronous (blocking) run on the CALLER's thread. #235-2：实际契约——
    // ConstraintWorkflow 在 worker 线程调用（constraintfactorjobs），依赖冻结
    // URI 快照 + worker 上独立建层 + nextRunDir 原子计数这一 QGIS 授权的后台
    // 处理形态；调用方不得传入主线程拥有的图层对象。注意 context 为默认构造，
    // 未注入工程 transformContext——需要 CRS 变换的算法接入前须先补注入。
    results = instance->run(params, context, &feedback, &ok);
  }
  catch (const QgsProcessingException &e)
  {
    setError(error, tr("algorithm '%1' raised an exception: %2").arg(algorithmId, e.what()));
    return QVariantMap();
  }
  catch (...)
  {
    setError(error, tr("algorithm '%1' raised an unexpected exception").arg(algorithmId));
    return QVariantMap();
  }

  if (!ok)
  {
    const QString log = feedback.textLog().trimmed();
    setError(error, tr("algorithm '%1' failed%2")
                      .arg(algorithmId, log.isEmpty() ? QString() : QStringLiteral(": %1").arg(log)));
    return QVariantMap();
  }
  return results;
}

QStringList QgisProcessingService::paleoAlgorithmIds() const
{
  QStringList ids;
  QgsProcessingRegistry *reg = QgsApplication::processingRegistry();
  if (!reg)
    return ids;

  // Registry ids are "providerId:algName"; the paleo provider's ids are
  // "paleo:<name>". A bare "paleo_*" prefix is also accepted so algorithms
  // registered under another provider still surface if named in-namespace.
  const QList<const QgsProcessingAlgorithm *> algs = reg->algorithms();
  for (const QgsProcessingAlgorithm *alg : algs)
  {
    const QString id = alg->id();
    if (id.startsWith(QStringLiteral("paleo:")) || id.startsWith(QStringLiteral("paleo_")))
      ids << id;
  }
  ids.sort();
  return ids;
}

QStringList QgisProcessingService::algorithmIds() const
{
  QStringList ids;
  QgsProcessingRegistry *reg = QgsApplication::processingRegistry();
  if (!reg)
    return ids;
  const QList<const QgsProcessingAlgorithm *> algs = reg->algorithms();
  for (const QgsProcessingAlgorithm *alg : algs)
    ids << alg->id();
  ids.removeDuplicates();
  ids.sort();
  return ids;
}

QWidget *QgisProcessingService::createAlgorithmDialog(const QString &algId,
                                                      const QVariantMap &presetParams,
                                                      QWidget *parent, QString *error)
{
  QgsProcessingRegistry *reg = QgsApplication::processingRegistry();
  const QgsProcessingAlgorithm *alg = reg ? reg->algorithmById(algId) : nullptr;
  if (!alg)
  {
    setError(error, tr("no processing algorithm registered as '%1'").arg(algId));
    return nullptr;
  }

  // create() runs initAlgorithm() — the widget needs a fully initialized
  // instance to build its parameter panel. Ownership moves to the widget.
  std::unique_ptr<QgsProcessingAlgorithm> instance;
  try
  {
    instance.reset(alg->create());
  }
  catch (const QgsProcessingException &e)
  {
    setError(error, tr("algorithm '%1' raised an exception: %2").arg(algId, e.what()));
    return nullptr;
  }
  catch (...)
  {
    setError(error, tr("algorithm '%1' raised an unexpected exception").arg(algId));
    return nullptr;
  }
  if (!instance)
  {
    setError(error, tr("algorithm '%1' could not create an instance").arg(algId));
    return nullptr;
  }

  // The widget host is always a QMainWindow in the QGIS 4.2 API. Passing
  // nullptr is legal: QgsDockableWidgetHelper then creates a free-standing
  // top-level QDialog (WidgetFlag::NoDocking forces dialog mode).
  auto *widget =
      new PaleoAlgorithmWidget(instance.release(), qobject_cast<QMainWindow *>(parent), m_project);
  if (!presetParams.isEmpty())
    widget->setParameters(presetParams);

  m_lastDialog = widget;
  return widget;
}

bool QgisProcessingService::showAlgorithmDialog(const QString &algId,
                                                const QVariantMap &presetParams,
                                                QWidget *parent, QString *error)
{
  QWidget *widget = createAlgorithmDialog(algId, presetParams, parent, error);
  if (!widget)
    return false;

  // Non-blocking show — no exec(), so this is safe under offscreen platforms.
  // With WidgetFlag::NoDocking the dockable helper already created a top-level
  // dialog at construction; showWidget() raises/activates it.
  static_cast<PaleoAlgorithmWidget *>(widget)->showWidget();
  return true;
}
