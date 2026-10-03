#pragma once

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <memory>

#include "../../src/io/dataimportservice.h"
#include "../../src/metadata/layermanifest.h"
#include "../../src/metadata/paleoprojectstore.h"
#include "../../src/qgis/qgislayerservice.h"
#include "../../src/qgis/qgisprojectservice.h"
#include "../../src/ui/datapreview/datapreviewtabs.h"

namespace paleo::tests::preview {

struct PreviewStack
{
  QgisProjectService projectSvc;
  std::unique_ptr<LayerManifest> manifest;
  std::unique_ptr<QgisLayerService> layerSvc;
  std::unique_ptr<PaleoProjectStore> store;
  std::unique_ptr<DataImportService> importSvc;
  std::unique_ptr<DataPreviewTabs> preview;
};

inline std::unique_ptr<PreviewStack> makeStack(const QString &projectDir)
{
  if (!QDir().mkpath(projectDir))
    return nullptr;
  auto s = std::make_unique<PreviewStack>();
  const QString metaPath = QDir(projectDir).filePath(QStringLiteral("metadata/project.sqlite"));
  if (!s->projectSvc.createProject(QDir(projectDir).filePath(QStringLiteral("proj.qgz"))))
    return nullptr;
  s->manifest = std::make_unique<LayerManifest>(metaPath);
  if (!s->manifest->open())
    return nullptr;
  s->layerSvc = std::make_unique<QgisLayerService>(&s->projectSvc, s->manifest.get());
  s->store = std::make_unique<PaleoProjectStore>();
  s->importSvc = std::make_unique<DataImportService>(s->store.get());
  QObject::connect(s->importSvc.get(), &DataImportService::layerDeclared,
                   s->layerSvc.get(), [layerSvc = s->layerSvc.get()](const LayerDeclaration &decl) {
                     QString err;
                     layerSvc->declare(decl, &err);
                   });
  s->importSvc->setProjectDir(projectDir);
  s->preview = std::make_unique<DataPreviewTabs>();
  s->preview->setImportService(s->importSvc.get());
  return s;
}

inline QString fixture(const QString &name)
{
  return QStringLiteral(PROJECT_FIXTURE_DIR) + QLatin1Char('/') + name;
}

inline QString stage(const QTemporaryDir &tmp, const QString &dir,
                                    const QString &name, const QString &asName = QString())
{
  const QString d = tmp.filePath(dir);
  if (!QDir().mkpath(d))
    return QString();
  const QString dst = QDir(d).filePath(asName.isEmpty() ? name : asName);
  return QFile::copy(fixture(name), dst) ? dst : QString();
}

struct Imported
{
  QString wellHead, las, tops, td, d61, sgy, png, pdf, geojson, sgySource;
};

inline Imported importAll(PreviewStack &st, const QTemporaryDir &tmp)
{
  Imported out;
  QString err;
  out.wellHead = st.importSvc->importProjectFile(fixture(QStringLiteral("ExportWellHead.dat")), &err);
  out.las = st.importSvc->importProjectFile(fixture(QStringLiteral("A1.Las")), &err);
  const QString topsPath = stage(tmp, QString::fromUtf8("井分层"), QStringLiteral("DC.dat"));
  out.tops = st.importSvc->importProjectFile(topsPath, &err);
  const QString tdPath = stage(tmp, QString::fromUtf8("时深"), QStringLiteral("A1_TD.dat"));
  out.td = st.importSvc->importProjectFile(tdPath, &err);
  const QString d61Path = stage(tmp, QString::fromUtf8("层位"), QStringLiteral("D61_sample.dat"),
                                QStringLiteral("D61.dat"));
  out.d61 = st.importSvc->importProjectFile(d61Path, &err);
  out.sgySource = tmp.filePath(QStringLiteral("vol.sgy"));
  QFile::copy(fixture(QStringLiteral("mini_seismic.sgy")), out.sgySource);
  out.sgy = st.importSvc->importProjectFile(out.sgySource, &err);
  out.png = st.importSvc->importProjectFile(fixture(QStringLiteral("tiny.png")), &err);
  out.pdf = st.importSvc->importProjectFile(fixture(QStringLiteral("tiny.pdf")), &err);
  out.geojson = st.importSvc->importProjectFile(fixture(QStringLiteral("facies.geojson")), &err);
  return out;
}

} // namespace paleo::tests::preview
