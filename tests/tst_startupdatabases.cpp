#include <QtTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QScopeGuard>
#include <QProcess>
#include "../src/app/appcontext.h"
#include "../src/qgis/qgisruntime.h"
#include "../src/qgis/qgisprojectservice.h"
#include "../src/ui/paleomainwindow.h"
#include "../src/ui/wellsection/wellsectionpanel.h"
#include "../src/metadata/paleoprojectstore.h"
#include "../src/metadata/wellsectionstore.h"
#include "../src/metadata/metastore.h"
#include <qgsapplication.h>
#include <qgsstyle.h>
#include <qgscolorrampimpl.h>
#include <qgsmessagelog.h>

class TestStartupDatabases : public QObject {
  Q_OBJECT
public:
  TestStartupDatabases(AppContext *ctx, QString executable) : m_ctx(ctx), m_executable(std::move(executable)) {}
private:
  AppContext *m_ctx;
  QString m_executable;
  std::unique_ptr<PaleoMainWindow> window() {
    auto w = std::make_unique<PaleoMainWindow>(m_ctx->canvasCtl(), m_ctx->projectSvc(),
        m_ctx->layerSvc(), m_ctx->toolSvc(), m_ctx->selection());
    w->attachWorkflows(m_ctx->predictionWf(), m_ctx->constraintWf(), m_ctx->compositionWf(),
        m_ctx->validationWf(), m_ctx->importSvc(), m_ctx->seismicLink(), m_ctx->processingSvc(),
        m_ctx->store(), m_ctx->editingSvc(), m_ctx->layoutSvc(), m_ctx->taskSvc());
    return w;
  }
private slots:
  void nativeDefaultColorRampsAreAvailable() {
    auto *style = QgsStyle::defaultStyle(); QVERIFY(style);
    QVERIFY(style->colorRampNames().contains(QStringLiteral("Viridis")));
    QVERIFY(style->colorRampNames().contains(QStringLiteral("Spectral")));
    QVERIFY(style->colorRampNames().contains(QStringLiteral("RdBu")));
  }
  void emptyAndCustomStyleLibrariesRepairWithoutOverwritingUserColors() {
    auto *style = QgsStyle::defaultStyle();
    const auto symbolCount = style->symbolCount();
    for (const auto &name : style->colorRampNames()) QVERIFY(style->removeColorRamp(name));
    QVERIFY(style->colorRampNames().isEmpty());
    QString error;
    QVERIFY2(QgisRuntime::ensureDefaultColorRamps(&error), qPrintable(error));
    QVERIFY(style->colorRampNames().contains("Spectral"));
    QVERIFY(!style->symbolsOfFavorite(QgsStyle::ColorrampEntity).isEmpty());
    QVERIFY(style->addColorRamp("Viridis", new QgsGradientColorRamp(Qt::red, Qt::blue), true));
    QVERIFY(style->addColorRamp("custom-geologist", new QgsGradientColorRamp(Qt::black, Qt::white), true));
    QVERIFY(style->addFavorite(QgsStyle::ColorrampEntity, "custom-geologist"));
    QVERIFY(style->removeColorRamp("Spectral"));
    QVERIFY2(QgisRuntime::ensureDefaultColorRamps(&error), qPrintable(error));
    QCOMPARE(style->colorRampRef("Viridis")->color(0), QColor(Qt::red));
    QCOMPARE(style->colorRampRef("custom-geologist")->color(1), QColor(Qt::white));
    QVERIFY(style->symbolsOfFavorite(QgsStyle::ColorrampEntity).contains("custom-geologist"));
    QCOMPARE(style->symbolCount(), symbolCount);
    const auto names = style->colorRampNames();
    QVERIFY2(QgisRuntime::ensureDefaultColorRamps(&error), qPrintable(error));
    QCOMPARE(style->colorRampNames(), names);
    QgsStyle reopened; QVERIFY2(reopened.load(QgsApplication::userStylePath()), qPrintable(reopened.errorString()));
    QCOMPARE(reopened.colorRampNames(), names);
    QCOMPARE(reopened.colorRampRef("Viridis")->color(0), QColor(Qt::red));
    QVERIFY(reopened.colorRampRef("Spectral"));
    for (const auto &name : style->symbolsOfFavorite(QgsStyle::ColorrampEntity))
      QVERIFY(style->removeFavorite(QgsStyle::ColorrampEntity, name));
    QVERIFY2(QgisRuntime::ensureDefaultColorRamps(&error), qPrintable(error));
    QVERIFY(style->symbolsOfFavorite(QgsStyle::ColorrampEntity).contains("Viridis"));
    QCOMPARE(style->colorRampNames(), names);
  }
  void invalidUserDatabaseFailsStartupWithPath() {
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("QT_FORCE_STDERR_LOGGING", "1");
    child.setProcessEnvironment(environment);
    // 嵌入运行时用 argv[0]="paleo" 创建 QApplication，Qt 的推断路径不是测试程序。
    child.start(m_executable, {"--invalid-user-db"});
    QVERIFY(child.waitForFinished(30000));
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QCOMPARE(child.exitCode(), 1);
    const auto error = child.readAllStandardError();
    QVERIFY2(error.contains("QGIS user database initialization failed:"), error.constData());
    QVERIFY(error.contains("qgis.db"));
  }
  void emptyUserDatabaseMigratesAndPreservesCustomCrs() {
    QVERIFY(m_ctx->ready());
    const auto name = QStringLiteral("startup-user-db-test");
    {
      auto db = QSqlDatabase::addDatabase("QSQLITE", name);
      db.setDatabaseName(QgsApplication::qgisUserDatabaseFilePath());
      QVERIFY(db.open());
      QSqlQuery q(db);
      QVERIFY2(q.exec("select srs_id from tbl_srs where srs_id >= 100000"), qPrintable(q.lastError().text()));
      QVERIFY(q.exec("insert into tbl_srs(srs_id,description,projection_acronym,ellipsoid_acronym,parameters,is_geo) "
                     "values(100001,'keep-custom-crs','utm','WGS84','+proj=utm +zone=49 +datum=WGS84',0)"));
      QString error;
      QVERIFY2(QgsApplication::createDatabase(&error), qPrintable(error));
      QVERIFY(q.exec("select description from tbl_srs where srs_id=100001"));
      QVERIFY(q.next()); QCOMPARE(q.value(0).toString(), QStringLiteral("keep-custom-crs"));
      db.close();
    }
    QSqlDatabase::removeDatabase(name);
  }
  void noProjectDoesNotOpenSectionStores() {
    QStringList messages;
    const auto c = connect(QgsApplication::messageLog(),
        &QgsMessageLog::messageReceivedWithFormat,
        this, [&messages](const QString &message, const QString &, Qgis::MessageLevel, Qgis::StringFormat) { messages << message; });
    const auto disconnectLog = qScopeGuard([&] { disconnect(c); });
    auto w = window();
    auto *panel = w->findChild<WellSectionPanel *>(QStringLiteral("wellSectionPanel"));
    QVERIFY(panel);
    panel->wellIdsChanged({"memory-only"});
    disconnect(c);
    for (const auto &message : messages) {
      QVERIFY2(!message.contains(QStringLiteral("未绑定工程库路径")), qPrintable(message));
      QVERIFY2(!message.contains(QStringLiteral("解释岩性来源读回失败")), qPrintable(message));
    }
  }
  void projectOpenCloseAndSwitchBindCorrectDatabase() {
    // catalog 会话由 AppContext 析构才完全释放；项目与 profile 一起在其后清理。
    QTemporaryDir dir(QgsApplication::qgisSettingsDirPath() + "projects-XXXXXX");
    dir.setAutoRemove(false);
    QStringList databasePaths;
    auto w = window();
    const auto closeDatabases = qScopeGuard([&] {
      m_ctx->projectSvc()->closeProject();
      for (const auto &path : databasePaths) MetaStore::closeConnectionsFor(path);
    });
    auto *panel = w->findChild<WellSectionPanel *>(QStringLiteral("wellSectionPanel"));
    QVERIFY(panel);
    QVERIFY(m_ctx->projectSvc()->createProject(dir.filePath("a.qgz")));
    const auto pathA = m_ctx->store()->metaDbPath();
    databasePaths << pathA;
    metadata::WellSectionStore savedA(pathA);
    { QSignalBlocker blocked(panel); panel->setWellIds({"well-a"}); }
    panel->wellIdsChanged({"well-a"});
    QString error;
    QCOMPARE(savedA.load("default", &error).wellIds, QStringList{"well-a"});
    QVERIFY2(error.isEmpty(), qPrintable(error));
    m_ctx->projectSvc()->closeProject();
    panel->wellIdsChanged({"closed-edit"});
    QCOMPARE(savedA.load("default", &error).wellIds, QStringList{"well-a"});
    QVERIFY(m_ctx->projectSvc()->createProject(dir.filePath("b.qgz")));
    const auto pathB = m_ctx->store()->metaDbPath();
    databasePaths << pathB;
    QVERIFY(pathA != pathB);
    metadata::WellSectionStore savedB(pathB);
    { QSignalBlocker blocked(panel); panel->setWellIds({"well-b"}); }
    panel->wellIdsChanged({"well-b"});
    QCOMPARE(savedB.load("default", &error).wellIds, QStringList{"well-b"});
    QCOMPARE(savedA.load("default", &error).wellIds, QStringList{"well-a"});
    m_ctx->projectSvc()->closeProject();
  }
};

int main(int argc, char **argv) {
  const auto executable = QFileInfo(QString::fromLocal8Bit(argv[0])).absoluteFilePath();
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QTemporaryDir settings;
  if (!settings.isValid()) return 1;
  qputenv("QGIS_CUSTOM_CONFIG_PATH", settings.path().toUtf8());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  // 复现已有 0 字节 qgis.db，而不是只测全新配置。
  const auto emptyDb = settings.filePath("profiles/default/qgis.db");
  QDir().mkpath(QFileInfo(emptyDb).absolutePath());
  QFile file(emptyDb);
  if (!file.open(QIODevice::WriteOnly)) return 1;
  if (argc > 1 && QByteArray(argv[1]) == "--invalid-user-db") file.write("not a SQLite database");
  file.close();
  AppContext ctx(QgisRuntime::defaultPrefixPath());
  if (!ctx.ready()) return 1;
  if (!QgsApplication::qgisUserDatabaseFilePath().startsWith(settings.path())) return 2;
  TestStartupDatabases test(&ctx, executable);
  return QTest::qExec(&test, argc, argv);
}
#include "tst_startupdatabases.moc"
