// 层：组装根
#include "appcontext.h"
#include "../workflow/wellfaciesworkflow.h"
#include "../ui/wellcomposite/wellcompositepanel.h"
#include "../ui/python/pythonconsolepanel.h" // 方向68：脚本面 importRequested 接线
#include "crossplotcontroller.h"
#include "../io/dataimportservice.h" // catalog() — attachMapping 的 OUTPUT 登记
#include "../io/wellcompositexml.h" // D1：wellcomposite 序列化/深度表解析注入（wave/deepen-perf）
#include "../ui/wellcomposite/derivedsink.h" // D1：派生登记 sink 默认实例
#include "../qgis/qgisruntime.h" // defaultPrefixPath——默认 prefix 跟构建链接面走
#include "../services/crashreport.h" // wave4：启动早期装崩溃处理器 + 脏退出提示
#include "../services/startuptrace.h" // goal/perf-systematize 簇1：启动分段仪表
#include "../ui/paleomainwindow.h"
#include "../ui/paleotheme.h" // T32：启动注册 vendor 字体 + 正文字体

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QSet>
#include <QStandardPaths>
#include <QString>
#include <QSurfaceFormat>
#include <QTranslator>
#include <cstdio>

namespace
{
  bool isChineseLocale(const QString &localeStr)
  {
    QString s = localeStr.trimmed();
    if (s.isEmpty())
      return false;

    // Strip codeset (.UTF-8) and modifier (@pinyin)
    s = s.section(QLatin1Char('.'), 0, 0);
    s = s.section(QLatin1Char('@'), 0, 0);
    s.replace(QLatin1Char('-'), QLatin1Char('_'));

    if (s.compare(QStringLiteral("chinese"), Qt::CaseInsensitive) == 0)
      return true;

    if (s.startsWith(QLatin1Char('_')) || s.endsWith(QLatin1Char('_')))
      return false;

    const QStringList parts = s.split(QLatin1Char('_'));
    if (parts.isEmpty())
      return false;

    // Primary language tag must be "zh"
    if (parts.first().compare(QStringLiteral("zh"), Qt::CaseInsensitive) != 0)
      return false;

    if (parts.size() == 1)
      return true; // "zh" alone

    static const QSet<QString> validScripts = {
        QStringLiteral("hans"), QStringLiteral("hant"), QStringLiteral("latn")
    };
    static const QSet<QString> validTerritories = {
        QStringLiteral("cn"), QStringLiteral("tw"), QStringLiteral("hk"),
        QStringLiteral("mo"), QStringLiteral("sg"), QStringLiteral("my")
    };

    if (parts.size() == 2)
    {
      const QString sub = parts.at(1).toLower();
      return validScripts.contains(sub) || validTerritories.contains(sub);
    }

    if (parts.size() == 3)
    {
      const QString script = parts.at(1).toLower();
      const QString terr = parts.at(2).toLower();
      return validScripts.contains(script) && validTerritories.contains(terr);
    }

    return false;
  }
} // namespace

// App entry. Order matters: QgisRuntime::initialize() (inside AppContext)
// creates the QgsApplication — which IS the process's QApplication — and must
// be the first QGIS/Qt app object. So there is deliberately no `QApplication
// app(argc, argv)` here; the app object comes from the runtime, and we exec on
// the instance once it exists.
int main(int argc, char *argv[])
{
  StartupTrace::mark(QStringLiteral("main_entry"));

  // Parse command line arguments early, before OpenGL, Qt resources, and crash handler.
  // Critical for --help / -h: terminates cleanly without installing CrashReport handlers,
  // preventing orphan .running session flags in <AppDataLocation>/crash/.
  QString qgisPrefix = qEnvironmentVariable("QGIS_PREFIX_PATH", QgisRuntime::defaultPrefixPath());
  QString targetPath;
  QString cliLocale;
  for (int i = 1; i < argc; ++i)
  {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h"))
    {
      std::printf("Usage: paleo [options] [project_path]\n"
                  "Options:\n"
                  "  --lang <locale>      Force UI language (e.g. zh_CN, en_US)\n"
                  "  --locale <locale>    Alias for --lang\n"
                  "  --prefix <path>      Set custom QGIS prefix directory\n"
                  "  --qgis-prefix <path> Set custom QGIS prefix directory\n"
                  "  --help, -h           Show this help message and exit\n");
      return 0;
    }
    else if (arg == QStringLiteral("--prefix") || arg == QStringLiteral("--qgis-prefix"))
    {
      if (i + 1 < argc)
        qgisPrefix = QString::fromLocal8Bit(argv[++i]);
      else
        std::fprintf(stderr, "paleo: warning: %s requires a path argument\n", qPrintable(arg));
    }
    else if (arg == QStringLiteral("--lang") || arg == QStringLiteral("--locale"))
    {
      if (i + 1 < argc)
        cliLocale = QString::fromLocal8Bit(argv[++i]);
      else
      {
        std::fprintf(stderr, "paleo: warning: %s requires a locale argument\n", qPrintable(arg));
        cliLocale = QStringLiteral("invalid");
      }
    }
    else if (arg.startsWith(QStringLiteral("--lang=")))
    {
      cliLocale = arg.mid(7);
    }
    else if (arg.startsWith(QStringLiteral("--locale=")))
    {
      cliLocale = arg.mid(9);
    }
    else if (!arg.startsWith(QLatin1Char('-')))
    {
      if (targetPath.isEmpty())
        targetPath = arg;
    }
  }

  // OpenGL 3.3 Core Profile default format must precede QApplication / QgsApplication
  // so that shared OpenGL contexts across the application (including QtWebEngine,
  // QGIS Map Canvas, and Seismic 3D Viewport) have compatible Core Profile contexts.
  QSurfaceFormat glFormat;
  glFormat.setVersion(3, 3);
  glFormat.setProfile(QSurfaceFormat::CoreProfile);
  glFormat.setDepthBufferSize(24);
  glFormat.setStencilBufferSize(8);
  glFormat.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
  QSurfaceFormat::setDefaultFormat(glFormat);

  // Initialize static Qt resources in paleo_core and paleo_ui
  Q_INIT_RESOURCE(seismic_shaders);
  Q_INIT_RESOURCE(translations);

  // Required by QtWebEngine when a process also owns OpenGL-backed widgets
  // (QGIS map canvas). Must precede the QApplication/QgsApplication ctor,
  // which lives inside QgisRuntime::initialize().
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

  // wave4/崩溃报告（§38 本地优先）：趁任何 QGIS/Qt 对象存在之前——落盘目录
  // 解析、致命信号处理器、.running 会话旗标一起就位；残留旗标=上次脏退出。
  // AppDataLocation 依应用名寻址，先钉住（QgsApplication 随后的改名不影响
  // 这里已解析出的路径）。
  QCoreApplication::setApplicationName(QStringLiteral("paleo_workbench"));
  QCoreApplication::setOrganizationName(QStringLiteral("paleo"));
  WellComposite::WellCompositePanel::setFaciesWorkflowFactory([](QObject *parent) {
    return new WellFaciesWorkflow(parent);
  });
  const CrashReport::SessionStart session = CrashReport::installCrashHandler(
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
  StartupTrace::mark(QStringLiteral("pre_qt_ready"));

  AppContext ctx(qgisPrefix); // brings up QgsApplication + wires all services
  if (!ctx.ready())
  {
    std::fprintf(stderr, "paleo: QgisRuntime::initialize failed for prefix '%s'\n",
                 qPrintable(qgisPrefix));
    return 1;
  }

  // --- Dynamic Locale & Translation Initialization (R4) ---
  // Precedence: CLI option (--lang/--locale) > PALEO_LOCALE > QLocale::system().name()
  QString requestedLocale;
  QString localeSource;
  if (!cliLocale.trimmed().isEmpty())
  {
    requestedLocale = cliLocale.trimmed();
    localeSource = QStringLiteral("CLI flag --lang");
  }
  else if (qEnvironmentVariableIsSet("PALEO_LOCALE") &&
           !qEnvironmentVariable("PALEO_LOCALE").trimmed().isEmpty())
  {
    requestedLocale = qEnvironmentVariable("PALEO_LOCALE").trimmed();
    localeSource = QStringLiteral("environment variable PALEO_LOCALE");
  }
  else
  {
    requestedLocale = QLocale::system().name();
    localeSource = QStringLiteral("system default (QLocale::system)");
  }

  if (isChineseLocale(requestedLocale))
  {
    // Configure default application locale for formatting
    QLocale::setDefault(QLocale(QLocale::Chinese, QLocale::China));

    // 1. Load Qt base translations (QMessageBox buttons, file dialogs, context menus)
    // Installed first so application-specific translations take precedence in search chain.
    auto *qtTranslator = new QTranslator(QCoreApplication::instance());
    const QString qtTransDir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (qtTranslator->load(QStringLiteral("qtbase_zh_CN"), qtTransDir) ||
        qtTranslator->load(QStringLiteral("qt_zh_CN"), qtTransDir))
    {
      QCoreApplication::installTranslator(qtTranslator);
      qInfo("paleo: Loaded Qt base translations from '%s'", qPrintable(qtTransDir));
    }
    else
    {
      qWarning("paleo: Qt base translations (qtbase_zh_CN.qm) not found in '%s'; dialog buttons may use English",
               qPrintable(qtTransDir));
      delete qtTranslator;
    }

    // 2. Load Paleo Workstation application translations from embedded resource
    auto *paleoTranslator = new QTranslator(QCoreApplication::instance());
    if (paleoTranslator->load(QStringLiteral(":/i18n/paleo_zh_CN.qm")))
    {
      QCoreApplication::installTranslator(paleoTranslator);
      qInfo("paleo: Loaded Chinese translation catalog from ':/i18n/paleo_zh_CN.qm' (locale '%s' via %s)",
            qPrintable(requestedLocale), qPrintable(localeSource));
    }
    else
    {
      qWarning("paleo: Failed to load Chinese translation catalog ':/i18n/paleo_zh_CN.qm'; falling back to English (en_US)");
      delete paleoTranslator;
    }
  }
  else
  {
    // Graceful fallback: non-Chinese locale requested, retain unmodified built-in English strings
    QLocale::setDefault(QLocale(requestedLocale));
    qInfo("paleo: Locale is '%s' (from %s); using built-in English (en_US)",
          qPrintable(requestedLocale), qPrintable(localeSource));
  }

  // T32 + 浅色默认主题：vendor 字体注册（缺失时 PaleoTheme 内如实告警降级）、
  // DESIGN.md 浅色 palette 钉死——不跟随系统深色模式。
  PaleoTheme::applyLightTheme();
  StartupTrace::mark(QStringLiteral("theme_ready"));

  PaleoMainWindow window(ctx.canvasCtl(), ctx.projectSvc(), ctx.layerSvc(),
                         ctx.toolSvc(), ctx.selection());
  QObject::connect(&ctx, &AppContext::projectReadOnlyChanged,
                   &window, &PaleoMainWindow::setProjectReadOnly);
  window.attachWorkflows(ctx.predictionWf(), ctx.constraintWf(),
                         ctx.compositionWf(), ctx.validationWf(), ctx.importSvc(),
                         ctx.seismicLink(), ctx.processingSvc(), ctx.store(),
                         ctx.editingSvc(), ctx.layoutSvc(), ctx.taskSvc());
  // goal/time-depth-velocity：层树深度域转换入口（壳接线在 AppContext 绑定
  // catalog 之后重绑生效——rebind 随工程打开）。
  window.attachDepthConversion(ctx.depthConversionWf());
  new paleo::crossplot::CrossplotController(&ctx, &window);
  // goal/property-modeling：地层格架属性建模编排接线
  window.attachPropertyModel(ctx.propertyModelWf(), ctx.faultCtl());
  // goal/facies-automapping：证据合成 + QA 报告面板（相图链编排接线）。
  window.attachFaciesMapping(ctx.faciesMappingWf());
  // 方向51：AI 地质对话助手（编排在 AppContext，面板只渲染 + 发意图）。
  window.attachAiAssistant(ctx.aiChat());
  // 方向68：Python 脚本面（服务/编排在 AppContext，面板只渲染 + 发意图）。
  // 结果导入意图在组装根接 DataImportService——视图不碰 io；未打开工程时
  // 导入如实失败并把原因回写控制台。
  if (auto *pythonPanel = window.attachPythonConsole(ctx.pythonConsole()))
  {
    QObject::connect(pythonPanel, &PythonConsolePanel::importRequested, &window,
                     [&ctx, pythonPanel](const QString &path) {
                       QString error;
                       const DataImportService::ImportResult result =
                           ctx.importSvc()->importProjectFileEx(path, &error);
                       QString message;
                       switch (result.outcome)
                       {
                         case DataImportService::ImportOutcome::Imported:
                           message = QCoreApplication::translate("PythonScripting",
                                                                 "已导入：%1")
                                         .arg(path);
                           break;
                         case DataImportService::ImportOutcome::AlreadyStored:
                           message = QCoreApplication::translate(
                                         "PythonScripting",
                                         "已在工程中（跳过重复导入）：%1")
                                         .arg(path);
                           break;
                         case DataImportService::ImportOutcome::Failed:
                           message = QCoreApplication::translate(
                                         "PythonScripting", "导入失败：%1——%2")
                                         .arg(path, error);
                           break;
                       }
                       pythonPanel->appendSystemLine(message);
                     });
  }
  // goal/fault-interpretation：剖面断层拾取/断层管理面板接编排器
  window.attachFaults(ctx.faultCtl());
  // 方向34：井网辅助（验证页「布井辅助」页签 + 地图布点工具）。
  window.attachWellSiting(ctx.wellsitingWf());
  // 方向64：错误呈现（通知卡/severe 模态/状态栏）+ 错误历史 dock。
  window.attachErrorHub(ctx.errorHub());
  // D1（wave/deepen-perf）：wellcomposite 派生登记/井斜时深装配的 io 注入——
  // 组装根是唯一可同时 include io/ 与 ui/ 的非视图目录（视图侧白名单只放
  // 行 io/lasdoc.h）。未注入时 sink 走诚实失败路径（状态栏+日志），不静默。
  if (auto *sink = WellComposite::WellCompositeDerivedSink::defaultSink())
  {
    sink->setSerializer([](const WellComposite::ComprehensiveWellData &doc,
                           const QStringList &auditLines) {
      return WellComposite::writeComprehensiveWellXml(doc, auditLines);
    });
    sink->setDepthTableParsers(
        [](const QString &path, QVector<WellComposite::DeviationStation> *out,
           QString *error) {
          QVector<WellComposite::XmlDeviationStation> parsed;
          if (!WellComposite::parseDeviationSurvey(path, parsed, error))
            return false;
          out->reserve(parsed.size());
          for (const auto &s : parsed)
            out->append({s.md, s.inclinationDeg, s.azimuthDeg});
          return true;
        },
        [](const QString &path, QVector<QPair<double, double>> *out,
           QString *error) {
          return WellComposite::parseTimeDepthTable(path, *out, error);
        });
  }
  window.attachMapping(ctx.mappingWf(), ctx.versionCtl(), ctx.versionStore(),
                       ctx.projectData(),
                       ctx.importSvc() ? ctx.importSvc()->catalog() : nullptr);
  window.attachWorkbench(ctx.mappingWorkbench());
  StartupTrace::mark(QStringLiteral("main_window_ready"));
  window.show();
  StartupTrace::mark(QStringLiteral("window_shown"));

  // goal/perf-systematize 簇1：首帧 paint 打点（事件循环内的首个主窗 Paint
  // ≈ 首帧上屏）。offscreen/真实平台都会对 shown 顶层窗投递 Paint。落盘
  // 一次；PALEO_STARTUP_EXIT_AFTER_FRAME=1 时测完即退（自动化口径，产品
  // 路径不设此 env）。
  class FirstPaintMarker : public QObject
  {
    public:
      explicit FirstPaintMarker(QObject *parent) : QObject(parent) {}
      bool eventFilter(QObject *watched, QEvent *ev) override
      {
        if (!m_done && ev->type() == QEvent::Paint)
        {
          m_done = true;
          StartupTrace::mark(QStringLiteral("first_paint"));
          StartupTrace::finish();
          if (qEnvironmentVariableIsSet("PALEO_STARTUP_EXIT_AFTER_FRAME"))
            QCoreApplication::exit(0);
          if (QObject *w = watched)
            w->removeEventFilter(this);
        }
        return false;
      }
    private:
      bool m_done = false;
  };
  auto *firstPaint = new FirstPaintMarker(&window);
  window.installEventFilter(firstPaint);

  if (!targetPath.isEmpty())
    window.openPath(targetPath);

  // §38 错误呈现契约——可恢复降级不模态：上次脏退出的诚实告知（有报告带
  // 路径，无报告如实说没有），不阻塞用户继续干活。
  if (const QString notice = CrashReport::recoveryNoticeText(session);
      !notice.isEmpty())
  {
    auto *box = new QMessageBox(&window);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(QObject::tr("Paleo 上次异常退出"));
    box->setText(notice);
    box->setModal(false);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->show();
  }

  // The QgsApplication built by the runtime is the live QApplication instance.
  const int exitCode = QApplication::exec();
  // #163：先排空任务池再让 window 析构（栈序：window 先于 ctx 析构）。worker
  // 闭包/finished 槽可能引用主窗口持有的对象（面板、JobRunner、workflow），
  // 旧实现等到 ~AppContext 才排空——那时 window 已析构，在途 worker 向已析构
  // 对象回包 = use-after-free。
  ctx.drainTasks();
  // 只有走到这里（正常退出）才清旗标；崩溃路径进程死在 exec 里，旗标残留
  // 正是下次启动脏退出检测的依据。
  CrashReport::clearRunningFlag();
  return exitCode;
}
