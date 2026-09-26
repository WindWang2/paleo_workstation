#include "qgisruntime.h"

#include <QCoreApplication>

#include <qgsapplication.h>
#include <qgsproviderregistry.h>

// P0 spine service — owns QgsApplication lifecycle and vendor prefix resolution.
//
// Init-order contract (all of it lives here, nowhere else):
//   1. Launcher may set env vars (QT_QPA_PLATFORM, QGIS_PREFIX, GDAL/PROJ data
//      dirs) before this call — after the ctor they are ignored by Qt/GDAL.
//   2. QgsApplication ctor (it is a QApplication; exactly one per process).
//   3. setPrefixPath() — vendor prefix or distro root ("/usr" on Arch). Must
//      precede initQgis so provider/plugin/resource paths resolve against it.
//   4. initQgis() — loads provider libs and locates srs.db.
//
// The QgsApplication is heap-allocated and owned by this runtime because its
// lifetime must equal the process lifetime — stack construction in main()
// would run ~QgsApplication before exitQgis() semantics are complete.
namespace
{
  QgsApplication *s_app = nullptr;
}

bool QgisRuntime::initialize( const QString &prefixPath )
{
  if ( isInitialized() )
    return false;

  // A Q(Core)Application already exists that we did not create: constructing a
  // second one aborts inside Qt. Caller must route its app through us instead.
  if ( QCoreApplication::instance() != nullptr )
    return false;

  // QgsApplication needs real argc/argv storage that outlives the ctor call;
  // the embedded binary's argv is not guaranteed to reach this layer.
  static int s_argc = 1;
  static char s_appName[] = "paleo";
  static char *s_argv[] = { s_appName, nullptr };

  s_app = new QgsApplication( s_argc, s_argv, /*GUIenabled=*/false );
  QgsApplication::setPrefixPath( qEnvironmentVariable( "QGIS_PREFIX_PATH", prefixPath ),
                                 /*useDefaultPaths=*/true );
  QgsApplication::initQgis();
  return true;
}

void QgisRuntime::shutdown()
{
  if ( !s_app )
    return;
  QgsApplication::exitQgis();
  delete s_app;
  s_app = nullptr;
}

bool QgisRuntime::isInitialized()
{
  // Deliberately not `s_app != nullptr`: QGIS counts as initialized if *any*
  // path (e.g. a test bootstrap) already created the QgsApplication.
  return QgsApplication::instance() != nullptr;
}

int QgisRuntime::providerCount()
{
  if ( !isInitialized() )
    return -1;
  return static_cast<int>( QgsProviderRegistry::instance()->providerList().size() );
}

QString QgisRuntime::srsDbPath()
{
  if ( !isInitialized() )
    return QString();
  return QgsApplication::srsDatabaseFilePath();
}
