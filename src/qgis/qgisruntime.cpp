// 层：QGIS 封装
#include "qgisruntime.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QSet>
#include <memory>
#include <cmath>
#include <ogr_spatialref.h>
#include <cpl_error.h>

#include <qgsapplication.h>
#include <qgscoordinatereferencesystem.h>
#include <qgscoordinatetransform.h>
#include <qgsexception.h>
#include <qgsproviderregistry.h>
#include <qgsgui.h>
#include <qgseditorwidgetregistry.h>
#include <qgsstyle.h>
#include <qgscolorramp.h>

// P0 spine service — owns QgsApplication lifecycle and vendor prefix resolution.
//
// Init-order contract (all of it lives here, nowhere else):
//   1. Launcher may set env vars (QT_QPA_PLATFORM, QGIS_PREFIX, GDAL/PROJ data
//      dirs) before this call — after the ctor they are ignored by Qt/GDAL.
//   2. QgsApplication ctor (it is a QApplication; exactly one per process).
//   3. setPrefixPath() — vendor prefix or distro root ("/usr" on Arch). Must
//      precede initQgis so provider/plugin/resource paths resolve against it.
//   4. init() — resolves the embedded application's user profile.
//   5. createDatabase() — creates/migrates qgis.db before user CRS queries.
//   6. initQgis() — loads provider libs and locates srs.db.
//
// The QgsApplication is heap-allocated and owned by this runtime because its
// lifetime must equal the process lifetime — stack construction in main()
// would run ~QgsApplication before exitQgis() semantics are complete.
namespace
{
  QgsApplication *s_app = nullptr;
}

bool QgisRuntime::initialize( const QString &prefixPath,
                              const std::function<void()> &applicationCreated )
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

  // GUIenabled=true：这是 GUI 工作站进程——false 会让 QgsApplication 进入
  // 无界面模式，window.show() 不生效、exec() 立即返回（进程静默退出）。
  // 测试路径不经过这里（各自直接构造 QgsApplication），不受影响。
  s_app = new QgsApplication( s_argc, s_argv, /*GUIenabled=*/true );
  QgsApplication::setPrefixPath(
      resolvePrefixPath( prefixPath, qEnvironmentVariable( "QGIS_PREFIX_PATH" ) ),
      /*useDefaultPaths=*/true );
  // 嵌入模式构造函数尚未解析 profile；先 init，否则用户库路径只是相对
  // qgis.db。initQgis() 加载 provider，不负责创建/迁移用户库。
  QgsApplication::init();
  // 在主题、面板或 provider 首次查询用户 CRS 前，使用 QGIS 原生迁移保留自定义 CRS。
  QString databaseError;
  if (!QgsApplication::createDatabase(&databaseError)) {
    qCritical().noquote() << "QGIS user database initialization failed:"
                          << QgsApplication::qgisUserDatabaseFilePath() << databaseError;
    delete s_app; s_app = nullptr;
    return false;
  }
  if ( applicationCreated )
    applicationCreated();
  QgsApplication::initQgis();
  // QGIS 4.2 CRS cache entries can outlive the worker which owns their PROJ
  // master object. Its TLS cleanup leaves shared entries valid but without a
  // projection (empty WKT), breaking later imports and map rendering. Keep
  // CRS instances independent across workers until that upstream lifetime
  // defect is fixed. Coordinate-transform caching remains enabled.
  QgsCoordinateReferenceSystem::invalidateCache(/*disableCache=*/true);
  // Embedding applications must register QGIS's native attribute editors;
  // initQgis() initializes providers, while QgisApp normally does this step.
  QgsGui::editorWidgetRegistry()->initEditors();
  QString styleError;
  if (!ensureDefaultColorRamps(&styleError))
    qWarning().noquote() << "QGIS default color ramp initialization failed:" << styleError;
  return true;
}

bool QgisRuntime::ensureDefaultColorRamps(QString *error)
{
  if (error) error->clear();
  const auto fail = [error](const QString &message) {
    if (error) *error = message;
    return false;
  };
  if (!isInitialized())
    return fail(QStringLiteral("QGIS is not initialized"));
  auto *target = QgsStyle::defaultStyle();
  QgsStyle defaults;
  const auto source = QgsApplication::defaultStylePath();
  if (!defaults.createMemoryDatabase() || !defaults.importXml(source))
    return fail(QStringLiteral("Cannot load QGIS color ramps from %1: %2").arg(source, defaults.errorString()));
  const auto names = target->colorRampNames();
  const QSet<QString> existing(names.begin(), names.end());
  const auto favorites = defaults.symbolsOfFavorite(QgsStyle::ColorrampEntity);
  for (const auto &name : defaults.colorRampNames()) {
    if (existing.contains(name)) continue;
    std::unique_ptr<QgsColorRamp> ramp(defaults.colorRamp(name));
    // 启动阶段先持久化再放入全局库；失败不能被后续内存命中伪装为已修复。
    if (!ramp || !target->saveColorRamp(name, ramp.get(), favorites.contains(name),
                                      defaults.tagsOfSymbol(QgsStyle::ColorrampEntity, name)))
      return fail(QStringLiteral("Cannot save color ramp %1 to %2: %3")
                      .arg(name, QgsApplication::userStylePath(), target->errorString()));
    target->addColorRamp(name, ramp.release());
  }
  // 有些旧库有色带却没有任何收藏，原生下拉首页因此仍为空。
  if (target->symbolsOfFavorite(QgsStyle::ColorrampEntity).isEmpty())
    for (const auto &name : favorites)
      if (!target->addFavorite(QgsStyle::ColorrampEntity, name))
        return fail(QStringLiteral("Cannot add favorite QGIS color ramp: %1").arg(name));
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

QJsonObject QgisRuntime::coordinateRuntimeReport()
{
  QJsonArray errors;
  QJsonObject checks;
  const auto check = [&](const QString &name, bool ok, const QString &detail) {
    checks.insert(name, ok);
    if (!ok) errors.append(detail);
  };
  check(QStringLiteral("srsDatabase"), QFileInfo(srsDbPath()).isReadable(),
        QStringLiteral("QGIS coordinate database is missing or unreadable: %1").arg(srsDbPath()));
  const QgsCoordinateReferenceSystem source(QStringLiteral("EPSG:32649"));
  const QgsCoordinateReferenceSystem target(QStringLiteral("EPSG:4326"));
  check(QStringLiteral("qgisEpsg32649"), source.isValid() && !source.toWkt().isEmpty(),
        QStringLiteral("QGIS cannot resolve EPSG:32649"));
  check(QStringLiteral("qgisEpsg4326"), target.isValid(), QStringLiteral("QGIS cannot resolve EPSG:4326"));
  bool qgisTransform = false;
  try {
    QgsCoordinateTransform transform(source, target, QgsCoordinateTransformContext());
    const auto point = transform.transform(QgsPointXY(233240.46, 3997621.12));
    qgisTransform = transform.isValid() && std::abs(point.x() - 108.037366) < .000001
        && std::isfinite(point.y());
  } catch (const QgsCsException &) {}
  check(QStringLiteral("qgisTransform"), qgisTransform, QStringLiteral("QGIS UTM49N to WGS84 transform failed"));
  OGRSpatialReference ogrSource, ogrTarget;
  const bool ogrCrs = ogrSource.importFromEPSG(32649) == OGRERR_NONE
      && ogrTarget.importFromEPSG(4326) == OGRERR_NONE;
  check(QStringLiteral("gdalEpsg32649"), ogrCrs, QString::fromUtf8(CPLGetLastErrorMsg()));
  bool gdalTransform = false;
  if (ogrCrs) {
    ogrSource.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    ogrTarget.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    auto *transform = OGRCreateCoordinateTransformation(&ogrSource, &ogrTarget);
    double x = 233240.46, y = 3997621.12;
    gdalTransform = transform && transform->Transform(1, &x, &y)
        && std::abs(x - 108.037366) < .000001 && std::isfinite(y);
    if (transform) OCTDestroyCoordinateTransformation(transform);
  }
  check(QStringLiteral("gdalTransform"), gdalTransform, QStringLiteral("GDAL UTM49N to WGS84 transform failed"));
  return {{"ok", errors.isEmpty()}, {"checks", checks}, {"errors", errors},
      {"executable", QCoreApplication::applicationFilePath()}, {"workingDirectory", QDir::currentPath()},
      {"qgisPrefix", QgsApplication::prefixPath()}, {"srsDatabase", srsDbPath()},
      {"userDatabase", QgsApplication::qgisUserDatabaseFilePath()},
      {"projData", qEnvironmentVariable("PROJ_DATA")}, {"projLib", qEnvironmentVariable("PROJ_LIB")},
      {"gdalData", qEnvironmentVariable("GDAL_DATA")}};
}
