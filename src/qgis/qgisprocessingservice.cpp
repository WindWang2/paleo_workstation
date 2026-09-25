#include "qgisprocessingservice.h"

#include "../algorithms/paleoalgorithms.h"
#include "../metadata/paleoprojectstore.h"

#include <QDir>
#include <QLabel>
#include <QMainWindow>
#include <QTemporaryDir>

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
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

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
                      ? QgsProject::instance()
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
        widgetContext.setProject(QgsProject::instance());
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
                                    QMainWindow *parentWindow)
        : QgsProcessingAlgorithmWidgetBase(
              parentWindow, QgsProcessingAlgorithmWidgetBase::WidgetMode::Single,
              QgsProcessingAlgorithmWidgetBase::WidgetFlag::NoDocking,
              Qgis::DockableWidgetInitialState::ForceDialog)
      {
        if (QgsProject *project = QgsProject::instance())
        {
          m_context.setProject(project);
          m_context.setTransformContext(project->transformContext());
        }
        setAlgorithm(algorithm);
        auto *panel = new PaleoAlgorithmParametersPanel(algorithm, this, &m_context);
        setMainWidget(panel);
      }

      ~PaleoAlgorithmWidget() override
      {
        // The in-flight task (if any) holds a raw pointer to the feedback
        // object — deleting it mid-run would be a use-after-free, so it is
        // intentionally leaked in that (unreachable-in-practice) corner.
        if (!m_running)
          delete m_feedback;
      }

      QVariantMap createProcessingParameters(
          QgsProcessingParametersGenerator::Flags flags =
              QgsProcessingParametersGenerator::Flags()) override
      {
        if (auto *panel = parametersPanel())
          return panel->createProcessingParameters(flags);
        return QVariantMap();
      }

      QgsProcessingContext *processingContext() override { return &m_context; }

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
        if (!algorithm()->checkParameterValues(params, m_context, &message))
        {
          // Message bar instead of QMessageBox: static QMessageBox helpers exec
          // a nested event loop, which hangs offscreen test runs.
          if (messageBar())
            messageBar()->pushMessage(tr("Unable to execute algorithm"), message,
                                      Qgis::MessageLevel::Warning);
          return;
        }

        delete m_feedback;
        m_feedback = createFeedback();

        applyContextOverrides(&m_context);
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
        auto *task = new QgsProcessingAlgRunnerTask(algorithm(), params, m_context, m_feedback);
        connect(task, &QgsProcessingAlgRunnerTask::executed, this,
                [this](bool successful, const QVariantMap &results) {
                  m_running = false;
                  finished(successful, results, m_context, m_feedback);
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

      QgsProcessingContext m_context;
      QgsProcessingFeedback *m_feedback = nullptr; // owned, freed between runs
      bool m_running = false;
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

QVariantMap QgisProcessingService::run(const QString &algorithmId, const QVariantMap &parameters, QString *error)
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
  QgsProcessingFeedback feedback;
  bool ok = false;
  QVariantMap results;
  try
  {
    // Synchronous (blocking) run — main thread only per QgsProcessingAlgorithm
    // contract; the async/task runner wraps this in its own thread boundary.
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
      new PaleoAlgorithmWidget(instance.release(), qobject_cast<QMainWindow *>(parent));
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
