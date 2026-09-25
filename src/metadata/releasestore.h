#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

#include "layermanifest.h"

// §44/发布语义 — ReleaseStore snapshots the manifest's declared layer set into
// the same project sqlite ("<qgz>.project.sqlite") under table `releases`.
// A release is an immutable, named capture of the FULL declared set at a point
// in time — the geological-versioning unit used by the validation/release
// workflow. Releases never mutate; diffing two releases yields the declared-set
// delta (added / removed / changed layer declarations).
struct ReleaseInfo {
  QString id;         // "rel-N", sequential per store
  QString name;       // user label, e.g. "v1.0-draft"
  QString note;       // free text
  QString createdUtc; // ISO-8601 UTC
  int layerCount = 0;
};

class ReleaseStore
{
  public:
    explicit ReleaseStore(const QString &metaSqlitePath);

    bool open(QString *error = nullptr); // creates schema if absent

    // Snapshot the given declared set as a new release. Returns the new
    // release id, or empty string (+error) on failure. Empty declared sets
    // are legal — an "empty project" baseline is a valid release.
    QString createRelease(const QString &name, const QString &note,
                          const QVector<LayerDeclaration> &decls,
                          QString *error = nullptr);

    QVector<ReleaseInfo> releases() const;                    // chronological
    QVector<LayerDeclaration> manifestAt(const QString &releaseId) const;

    // Field-level delta between two snapshots (by layerId):
    // added = in B only; removed = in A only; changed = present in both but
    // horizon/type/source/styleRef/group differ.
    bool diff(const QString &idA, const QString &idB,
              QStringList *added, QStringList *removed, QStringList *changed) const;

  private:
    QString m_dbPath;
};
