#include "qgisprocessingservice.h"

#include "../algorithms/paleoalgorithms.h"
#include "../metadata/paleoprojectstore.h"

#include <QDir>
#include <QTemporaryDir>

#include <atomic>
#include <memory>

#include <qgsexception.h>
#include <qgsapplication.h>
#include <qgsprocessingalgorithm.h>
#include <qgsprocessingcontext.h>
#include <qgsprocessingfeedback.h>
#include <qgsprocessingparameters.h>
#include <qgsprocessingregistry.h>

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
