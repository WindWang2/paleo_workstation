// 层：组装根
#include "appcontext.h"
#include "../io/dataimportservice.h" // catalog() — attachMapping 的 OUTPUT 登记
#include "../services/crashreport.h" // wave4：启动早期装崩溃处理器 + 脏退出提示
#include "../ui/paleomainwindow.h"
#include "../ui/paleotheme.h" // T32：启动注册 vendor 字体 + 正文字体

#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include <QStandardPaths>
#include <QString>
#include <QSurfaceFormat>
#include <cstdio>

// App entry. Order matters: QgisRuntime::initialize() (inside AppContext)
// creates the QgsApplication — which IS the process's QApplication — and must
// be the first QGIS/Qt app object. So there is deliberately no `QApplication
// app(argc, argv)` here; the app object comes from the runtime, and we exec on
// the instance once it exists.
int main(int argc, char *argv[])
{
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

  // Initialize static Qt resources in paleo_core
  Q_INIT_RESOURCE(seismic_shaders);

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
  const CrashReport::SessionStart session = CrashReport::installCrashHandler(
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));

  QString qgisPrefix = qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr"));
  QString targetPath;
  for (int i = 1; i < argc; ++i)
  {
    const QString arg = QString::fromLocal8Bit(argv[i]);
    if (arg == QStringLiteral("--prefix") || arg == QStringLiteral("--qgis-prefix"))
    {
      if (i + 1 < argc)
        qgisPrefix = QString::fromLocal8Bit(argv[++i]);
    }
    else if (!arg.startsWith(QLatin1Char('-')))
    {
      if (targetPath.isEmpty())
        targetPath = arg;
    }
  }

  AppContext ctx(qgisPrefix); // brings up QgsApplication + wires all services
  if (!ctx.ready())
  {
    std::fprintf(stderr, "paleo: QgisRuntime::initialize failed for prefix '%s'\n",
                 qPrintable(qgisPrefix));
    return 1;
  }

  // T32 + 浅色默认主题：vendor 字体注册（缺失时 PaleoTheme 内如实告警降级）、
  // DESIGN.md 浅色 palette 钉死——不跟随系统深色模式。
  PaleoTheme::applyLightTheme();

  PaleoMainWindow window(ctx.canvasCtl(), ctx.projectSvc(), ctx.layerSvc(),
                         ctx.toolSvc(), ctx.selection());
  window.attachWorkflows(ctx.predictionWf(), ctx.constraintWf(),
                         ctx.compositionWf(), ctx.validationWf(), ctx.importSvc(),
                         ctx.seismicLink(), ctx.processingSvc(), ctx.store(),
                         ctx.editingSvc(), ctx.layoutSvc(), ctx.taskSvc());
  window.attachMapping(ctx.mappingWf(), ctx.versionCtl(), ctx.versionStore(),
                       ctx.projectData(),
                       ctx.importSvc() ? ctx.importSvc()->catalog() : nullptr);
  window.show();
  if (!targetPath.isEmpty())
    window.openPath(targetPath);

  // §38 错误呈现契约——可恢复降级不模态：上次脏退出的诚实告知（有报告带
  // 路径，无报告如实说没有），不阻塞用户继续干活。
  if (const QString notice = CrashReport::recoveryNoticeText(session);
      !notice.isEmpty())
  {
    auto *box = new QMessageBox(&window);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(QStringLiteral("Paleo 上次异常退出"));
    box->setText(notice);
    box->setModal(false);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->show();
  }

  // The QgsApplication built by the runtime is the live QApplication instance.
  const int exitCode = QApplication::exec();
  // 只有走到这里（正常退出）才清旗标；崩溃路径进程死在 exec 里，旗标残留
  // 正是下次启动脏退出检测的依据。
  CrashReport::clearRunningFlag();
  return exitCode;
}
