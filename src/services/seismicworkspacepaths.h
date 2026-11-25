// 层：数据
#pragma once
#include <QString>
struct CatalogEntity;

namespace seismic {
struct WorkspaceLocations {
  QString chunkBase, chunkMeta, pagedFile;
  QString readyChunk, readyPaged;
  bool prepareOutput() const;
  QString ownerProject, projectDir, relativeDirectory, sourcePath;
  qint64 sourceBytes = 0, sourceTicks = 0;
};

// Existing source-side companions are read-only. New output is isolated by
// catalog version and source file stamp under the active project's cache.
WorkspaceLocations workspaceLocations(const QString &projectDir, const QString &versionId,
                                     const QString &sourcePath, const CatalogEntity *survey = nullptr);
}
