#pragma once
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

class QgisLayerService;
class PaleoProjectStore;

// io/ — DataImportService ingests external files into the project.
// §41.2: files are copied into <project>/data/<kind>/ via the store's write
// queue (never direct FS writes from UI); a DataAsset record + manifest
// LayerDeclaration are produced so the import is lazy-loadable and validated.
class DataImportService : public QObject
{
  Q_OBJECT
  public:
    DataImportService(QgisLayerService *layers, PaleoProjectStore *store, QObject *parent = nullptr);
    void setProjectDir(const QString &dir);   // where the project lives

    // kind: "wells" | "seismic" | "boundary" | "raster".
    // Copies file under data/<kind>/, declares a manifest layer, registers the
    // asset record. Returns asset id or empty + error.
    QString importFile(const QString &kind, const QString &sourcePath, QString *error = nullptr);

    QStringList assets(const QString &kind = QString()) const;   // asset ids
    QString assetSource(const QString &assetId) const;           // project-relative path

  signals:
    void imported(const QString &kind, const QString &assetId, const QString &layerId);
    void importFailed(const QString &kind, const QString &path, const QString &error);

  private:
    QgisLayerService *m_layers;
    PaleoProjectStore *m_store;
    QString m_projectDir;
    QList<QPair<QString, QString>> m_assets; // (assetId, kind)
    QHash<QString, QString> m_assetSource;
    int m_seq = 0;
};
