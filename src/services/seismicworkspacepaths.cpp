// 层：数据
#include "catalog/datacatalog.h"
#include "seismicworkspacepaths.h"
#include "fspathutils.h"
#include "metadata/managedpath.h"
#include "Engine/PagedWorkspace.h"
#include "Engine/WorkspaceFormat.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <filesystem>

namespace seismic {
namespace {
bool matchesPaged(const QString &candidate, const std::filesystem::path &source,
                  quint64 bytes, qint64 ticks, const CatalogEntity *survey)
{
  if (!QFileInfo(candidate).isFile()) return false;
  engine::PagedWorkspaceReader reader;
  std::string error;
  if (!reader.Open(paleo::toFsPath(candidate), error, true)) return false;
  const auto &info = reader.Info();
  if (survey && (info.coverageInlineMin != survey->inlineMin || info.coverageInlineMax != survey->inlineMax ||
                 info.coverageXlineMin != survey->xlineMin || info.coverageXlineMax != survey->xlineMax)) return false;
  return info.complete && info.lodLevel == 0 && info.sourceSize == bytes &&
         info.sourceMtimeTicks == ticks && info.sourceFingerprint == engine::PagedWorkspaceFingerprint(source, bytes);
}

bool matchesChunk(const QString &base, quint64 bytes, qint64 ticks)
{
  if (!QFileInfo::exists(base + QStringLiteral(".sf3c.meta"))) return false;
  engine::WorkspaceMetaSummary summary;
  std::string error;
  return engine::ProbeWorkspaceMeta(paleo::toFsPath(base), summary, error) &&
         summary.readable && summary.complete &&
         summary.sourceIdentityHash == (bytes ^ (quint64(ticks) << 1));
}
}

WorkspaceLocations workspaceLocations(const QString &projectDir, const QString &versionId, const QString &sourcePath, const CatalogEntity *survey)
{
  WorkspaceLocations paths;
  const QFileInfo file(sourcePath);
  if (!file.isFile() || !file.isReadable()) return paths;
  const auto source = paleo::toFsPath(sourcePath);
  std::error_code error;
  const qint64 ticks = std::filesystem::last_write_time(source, error).time_since_epoch().count();
  if (error) return paths;
  const auto size = quint64(file.size());
  paths.sourcePath = sourcePath;
  paths.sourceBytes = file.size();
  paths.sourceTicks = ticks;
  paths.ownerProject = projectDir;
  if (QFileInfo(projectDir).isDir() && !projectDir.isEmpty() && paleo::managedpath::isSafePathSegment(versionId)) {
    paths.relativeDirectory = QStringLiteral("artifacts/cache/seismic/%1/%2-%3").arg(versionId).arg(size).arg(ticks);
    paths.projectDir = paleo::managedpath::workLocationRoot(projectDir, paths.relativeDirectory);
    const QString base = paths.relativeDirectory + QStringLiteral("/volume");
    paths.chunkBase = paleo::managedpath::resolveManagedPath(paths.projectDir, base);
    paths.chunkMeta = paleo::managedpath::resolveManagedPath(paths.projectDir, base + QStringLiteral(".sf3c.meta"));
    paths.pagedFile = paleo::managedpath::resolveManagedPath(paths.projectDir, base + QStringLiteral(".sf3p"));
  }
  if (survey && survey->entityType != QLatin1String("seismic_survey")) survey = nullptr;
  for (const auto &p : {paths.pagedFile, sourcePath + QStringLiteral(".sf3p")}) {
    if (p != paths.pagedFile && !survey) continue; // legacy ROI may be partial
    if (!p.isEmpty() && matchesPaged(p, source, size, ticks, survey)) { paths.readyPaged = p; break; }
  }
  for (const auto &p : {paths.chunkBase, sourcePath}) {
    if (!p.isEmpty() && matchesChunk(p, size, ticks)) { paths.readyChunk = p + QStringLiteral(".sf3c.meta"); break; }
  }
  return paths;
}

bool WorkspaceLocations::prepareOutput() const
{
  if (!QFileInfo(ownerProject).isDir() || chunkBase.isEmpty() || chunkMeta.isEmpty() || pagedFile.isEmpty()) return false;
  std::error_code error;
  const auto currentTicks = std::filesystem::last_write_time(paleo::toFsPath(sourcePath), error).time_since_epoch().count();
  if (error || currentTicks != sourceTicks || QFileInfo(sourcePath).size() != sourceBytes) return false;
  const QString dir = paleo::managedpath::resolveManagedPath(projectDir, relativeDirectory);
  if (dir.isEmpty() || !QDir().mkpath(dir)) return false;
  // Check every existing member, including incomplete shards and LOD sidecars,
  // before passing the directory to the engine which may resume or replace them.
  for (const auto &member : QDir(dir).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
    if (member.isDir() || paleo::managedpath::resolveManagedPath(projectDir, relativeDirectory + '/' + member.fileName()).isEmpty())
      return false;
  }
  return paleo::managedpath::resolveManagedPath(projectDir, relativeDirectory) == dir;
}
} // namespace seismic
