#include "appcontext.h"
#include "../io/dataimportservice.h" // catalog() — attachMapping 的 OUTPUT 登记
#include "../ui/paleomainwindow.h"
#include "../ui/paleotheme.h" // T32：启动注册 vendor 字体 + 正文字体

#include <QApplication>
#include <QCoreApplication>
#include <QString>
#include <cstdio>

// App entry. Order matters: QgisRuntime::initialize() (inside AppContext)
// creates the QgsApplication — which IS the process's QApplication — and must
// be the first QGIS/Qt app object. So there is deliberately no `QApplication
// app(argc, argv)` here; the app object comes from the runtime, and we exec on
// the instance once it exists.
int main(int argc, char *argv[])
{
  // Required by QtWebEngine when a process also owns OpenGL-backed widgets
  // (QGIS map canvas). Must precede the QApplication/QgsApplication ctor,
  // which lives inside QgisRuntime::initialize().
  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

  const QString qgisPrefix =
      argc > 1 ? QString::fromLocal8Bit(argv[1])
               : qEnvironmentVariable("QGIS_PREFIX_PATH", QStringLiteral("/usr"));

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

  // The QgsApplication built by the runtime is the live QApplication instance.
  return QApplication::exec();
}
