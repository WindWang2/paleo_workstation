#include <QHash>  // must precede the header: m_assetSource is a QHash member (fwd-decl only there)
#include <QList>  // ditto for m_assets
#include <QPair>
#include "dataimportservice.h"

#include "../metadata/layermanifest.h"
#include "../metadata/paleoprojectstore.h"
#include "../qgis/qgislayerservice.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

// ---------------------------------------------------------------------------
// §41.2 — ingest contract:
//   validate source → copy to <project>/data/<kind>/<basename> through the
//   store's serialized write queue → declare a manifest layer (source is the
//   project-absolute path; raster only for .tif/.img) → register the asset.
// Any failure emits importFailed() and returns an empty asset id.
// ---------------------------------------------------------------------------
namespace
{
  void setError(QString *error, const QString &text)
  {
    if (error)
      *error = text;
  }

  QString layerTypeFor(const QString &path)
  {
    const QString ext = QFileInfo(path).suffix().toLower();
    return (ext == QStringLiteral("tif") || ext == QStringLiteral("img"))
               ? QStringLiteral("raster")
               : QStringLiteral("vector");
  }
} // namespace

DataImportService::DataImportService(QgisLayerService *layers, PaleoProjectStore *store, QObject *parent)
  : QObject(parent)
  , m_layers(layers)
  , m_store(store)
{
}

void DataImportService::setProjectDir(const QString &dir)
{
  m_projectDir = dir;
}

QString DataImportService::importFile(const QString &kind, const QString &sourcePath, QString *error)
{
  const auto fail = [&](const QString &msg) -> QString {
    setError(error, msg);
    emit importFailed(kind, sourcePath, msg);
    return QString();
  };

  if (!m_layers || !m_store)
    return fail(QStringLiteral("import service is not fully wired"));
  if (kind.isEmpty())
    return fail(QStringLiteral("import kind is empty"));
  if (m_projectDir.isEmpty())
    return fail(QStringLiteral("project dir is not set"));
  if (sourcePath.isEmpty() || !QFile::exists(sourcePath))
    return fail(QStringLiteral("source file does not exist: %1").arg(sourcePath));

  const QString relPath = QStringLiteral("data/%1/%2").arg(kind, QFileInfo(sourcePath).fileName());
  const QString dst = QDir(m_projectDir).absoluteFilePath(relPath); // project-absolute

  // Serialized copy — UI never mutates the project tree directly.
  const PaleoProjectStore::WriteResult wr =
      m_store->enqueueWrite([sourcePath, dst]() -> PaleoProjectStore::WriteResult {
        if (QFileInfo(sourcePath).absoluteFilePath() == QFileInfo(dst).absoluteFilePath())
          return {true, QString()}; // already in place
        const QDir dir = QFileInfo(dst).absoluteDir();
        if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
          return {false, QStringLiteral("cannot create directory %1").arg(dir.absolutePath())};
        if (QFile::exists(dst) && !QFile::remove(dst))
          return {false, QStringLiteral("cannot replace existing %1").arg(dst)};
        if (!QFile::copy(sourcePath, dst))
          return {false, QStringLiteral("failed to copy %1 to %2").arg(sourcePath, dst)};
        return {true, QString()};
      });
  if (!wr.ok)
    return fail(wr.error.isEmpty() ? QStringLiteral("copy into project failed") : wr.error);

  const int seq = ++m_seq;
  const QString assetId = QStringLiteral("%1-%2").arg(kind).arg(seq);
  const QString layerId = QStringLiteral("%1.%2").arg(kind).arg(seq);

  LayerDeclaration decl;
  decl.layerId = layerId;
  decl.type = layerTypeFor(sourcePath);
  decl.source = dst; // provider source is the project-absolute copy
  decl.group = QStringLiteral("00_Data");

  QString derr;
  if (!m_layers->declare(decl, &derr))
    return fail(derr.isEmpty() ? QStringLiteral("manifest declare failed for %1").arg(layerId) : derr);

  m_assets.append({assetId, kind});
  m_assetSource.insert(assetId, relPath);

  emit imported(kind, assetId, layerId);
  return assetId;
}

QStringList DataImportService::assets(const QString &kind) const
{
  QStringList out;
  for (const QPair<QString, QString> &a : m_assets)
    if (kind.isEmpty() || a.second == kind)
      out.append(a.first);
  return out;
}

QString DataImportService::assetSource(const QString &assetId) const
{
  return m_assetSource.value(assetId); // project-relative "data/<kind>/<file>"
}
