#include "appcontext.h"

#include "../qgis/qgisruntime.h"
#include "../qgis/qgiscanvascontroller.h"
#include "../qgis/qgisprojectservice.h"
#include "../qgis/qgislayerservice.h"
#include "../qgis/qgisprocessingservice.h"
#include "../qgis/qgiseditingservice.h"
#include "../qgis/qgisstyleservice.h"
#include "../services/toolavailability.h"
#include "../linkage/selectioncontext.h"
#include "../linkage/seismicmaplink.h"
#include "../metadata/paleoprojectstore.h"
#include "../metadata/layermanifest.h"
#include "../qgis/manifestprojection.h"
#include "../io/dataimportservice.h"
#include "../qgis/qgislayoutservice.h"
#include "../workflow/workflows.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QDebug>

// Composition root. QgisRuntime::initialize() must run before any Qgs*
// construction (it owns the QgsApplication), so it is the very first thing the
// ctor does; everything else hangs off `this` as a QObject child and dies in
// the dtor before QgisRuntime::shutdown().
//
// Manifest: §37 declares the manifest the layer-set authority, persisted per
// project at "<qgz>.project.sqlite" (interim convention; the full
// metadata/project.sqlite layout from §4 lands with the store migration). A
// LayerManifest object exists from ctor — bound to an empty path, never opened
// — so QgisLayerService captures a permanently valid pointer. On
// projectOpened the object is copy-assigned in place to the real path
// (LayerManifest is a plain value type over a QString path, and the sqlite
// connection is looked up lazily per path in layermanifest.cpp), then opened.
namespace
{
  // Which AppContext instances performed the initialize() — mirrors the
  // file-scope guard pattern in qgiscanvascontroller.cpp so the header's
  // member set stays untouched. Only the initializing context shuts down.
  QSet<AppContext *> s_runtimeOwners;

  QString manifestPathFor(const QString &qgzPath)
  {
    return qgzPath + QStringLiteral(".project.sqlite");
  }

  QString gpkgPathFor(const QString &qgzPath)
  {
    const QFileInfo fi(qgzPath);
    return fi.absoluteDir().filePath(fi.completeBaseName() + QStringLiteral(".gpkg"));
  }
} // namespace

AppContext::AppContext(const QString &qgisPrefix, QObject *parent)
  : QObject(parent)
{
  // QgsApplication is a QApplication: initialize() creates it, and it must be
  // the process's only Q(Core)Application. If some bootstrap already brought
  // the runtime up we are ready but do not own its shutdown.
  if (QgisRuntime::isInitialized())
  {
    m_ready = true;
  }
  else if (QgisRuntime::initialize(qgisPrefix))
  {
    s_runtimeOwners.insert(this);
    m_ready = true;
  }

  if (!m_ready)
  {
    qWarning() << "AppContext: QgisRuntime not initialized (prefix" << qgisPrefix
               << ") — all service accessors will return nullptr";
    return;
  }

  m_store = new PaleoProjectStore(this);
  m_projectSvc = new QgisProjectService(this);

  // Deferred binding: constructed un-opened on an empty path; rebound (same
  // object, same pointer the layer service holds) when a project opens.
  m_manifest = new LayerManifest(QString());
  m_layerSvc = new QgisLayerService(m_projectSvc, m_manifest, this);

  // §37: every project write embeds the full declared layer set so
  // uninstantiated declarations survive the .qgz projection.
  m_projectSvc->setDeclarationProvider([this]() { return m_layerSvc->declared(); });

  m_import = new DataImportService(m_layerSvc, m_store, this);

  m_canvasCtl = new QgisCanvasController(this);
  m_procSvc = new QgisProcessingService(m_store, this);
  m_editSvc = new QgisEditingService(m_store, this);
  m_styleSvc = new QgisStyleService(this);
  m_toolSvc = new ToolAvailabilityService(m_store, this);
  m_selection = new SelectionContext(this);

  // 地震—地图联动: binds the selection context to the canvas. Forcing canvas()
  // here materializes the widget early; the main window reparents it into the
  // center stack. The dynamic property publishes the context so UI panels
  // (SeismicPreviewPanel) can resolve it without a ctx getter on the link.
  m_seismicLink = new SeismicMapLink(m_canvasCtl->canvas(), m_selection, this);
  m_seismicLink->setProperty("paleo.seismic.ctx",
                             QVariant::fromValue<QObject *>(m_selection));

  // Workflow orchestrators — thin bindings over the services above.
  // Layout service rides the service-owned QgsProject (eager — valid already).
  m_layoutSvc = new QgisLayoutService(m_projectSvc->project(), this);

  // Workflow orchestrators — thin bindings over the services above.
  m_predictionWf = new PredictionWorkflow(m_procSvc, m_layerSvc, this);
  m_constraintWf = new ConstraintWorkflow(m_procSvc, m_layerSvc, this);
  m_constraintWf->setStore(m_store); // GeoPackage constraint persistence (wave/constraint-gpkg)
  m_compositionWf = new CompositionWorkflow(m_procSvc, m_layerSvc, this);
  m_validationWf = new ValidationWorkflow(m_layerSvc, m_store, this);

  // ensureManifest-on-open: first point a per-project path is derivable.
  connect(m_projectSvc, &QgisProjectService::projectOpened, this,
          [this](const QString &qgzPath) {
            const QFileInfo fi(qgzPath);
            const QString metaPath = manifestPathFor(qgzPath);
            m_store->setProjectPaths(qgzPath, gpkgPathFor(qgzPath), metaPath);

            // §37 recovery: the SQLite manifest is authoritative, but the .qgz
            // carries an embedded copy of the declared set. Rehydrate ONLY when
            // the store file does not exist — a .qgz moved/shared without its
            // sidecar must not lose its layer set, while an existing store
            // (even a deliberately emptied one) is never overwritten.
            const bool metaExisted = QFileInfo::exists(metaPath);

            *m_manifest = LayerManifest(metaPath); // rebind in place
            QString err;
            if (!m_manifest->open(&err))
              qWarning() << "AppContext: failed to open layer manifest" << metaPath << err;

            if (!metaExisted)
            {
              const QVector<LayerDeclaration> embedded =
                  ManifestProjection::extractDeclarations(m_projectSvc->project());
              for (const LayerDeclaration &d : embedded)
              {
                LayerDeclaration copy = d;
                copy.instantiated = false; // runtime flag — layer service decides
                if (!m_manifest->upsert(copy, &err))
                  qWarning() << "AppContext: manifest rehydrate failed for" << d.layerId << err;
              }
              if (!embedded.isEmpty())
                qInfo() << "AppContext: manifest store missing — restored"
                        << embedded.size() << "declarations from .qgz projection";
            }

            m_styleSvc->setStylesRoot(fi.absoluteDir().filePath(QStringLiteral("styles")));
            m_import->setProjectDir(fi.absolutePath());
          });
}

AppContext::~AppContext()
{
  // Services embed Qgs* objects whose destruction can touch providers —
  // they must die while the runtime is still up. They are all direct
  // children; delete them explicitly before shutdown instead of leaving
  // them to ~QObject (which runs after this body).
  const QObjectList kids = children();
  for (QObject *kid : kids)
    delete kid;

  delete m_manifest; // not a QObject — plain path-holding value type
  m_manifest = nullptr;

  if (s_runtimeOwners.remove(this))
    QgisRuntime::shutdown();
}
