#include "appcontext.h"
#include "../ui/paleomainwindow.h"

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
  const QString qgisPrefix =
      argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("/usr");

  AppContext ctx(qgisPrefix); // brings up QgsApplication + wires all services
  if (!ctx.ready())
  {
    std::fprintf(stderr, "paleo: QgisRuntime::initialize failed for prefix '%s'\n",
                 qPrintable(qgisPrefix));
    return 1;
  }

  PaleoMainWindow window(ctx.canvasCtl(), ctx.projectSvc(), ctx.layerSvc(),
                         ctx.toolSvc(), ctx.selection());
  window.attachWorkflows(ctx.predictionWf(), ctx.constraintWf(),
                         ctx.compositionWf(), ctx.validationWf(), ctx.importSvc(),
                         ctx.seismicLink());
  window.show();

  // The QgsApplication built by the runtime is the live QApplication instance.
  return QApplication::exec();
}
