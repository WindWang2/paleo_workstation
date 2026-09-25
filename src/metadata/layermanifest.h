#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

// §37 — declared-layer manifest is the layer-SET authority (not .qgz).
// Persisted in metadata/project.sqlite table `layer_declarations`.
// A declaration exists independent of whether its QgsMapLayer is instantiated.
struct LayerDeclaration {
  QString layerId;      // stable id, e.g. "facies.T1"
  QString horizon;      // horizon this layer instance belongs to ("" = horizon-agnostic)
  QString type;         // "vector" | "raster" | "annotations" ...
  QString source;       // provider URI (gpkg table / raster path / memory)
  QString styleRef;     // style identifier in styles/
  QString group;        // layer-tree group, e.g. "04_SingleFactor"
  bool instantiated = false; // runtime-only, not persisted
};

// Manifest persistence over the project.sqlite metadata store.
class LayerManifest
{
  public:
    explicit LayerManifest(const QString &metaSqlitePath);

    bool open(QString *error = nullptr);       // creates schema if absent
    bool upsert(const LayerDeclaration &decl, QString *error = nullptr);
    bool remove(const QString &layerId, QString *error = nullptr);
    QVector<LayerDeclaration> all() const;                       // full declared set
    QVector<LayerDeclaration> forHorizon(const QString &h) const;
    QStringList horizons() const;

  private:
    QString m_dbPath;
};
