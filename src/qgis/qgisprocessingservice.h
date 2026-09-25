#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>

class QgsProcessingContext;
class QgsProcessingFeedback;
class PaleoProjectStore;

// qgis/ — QgisProcessingService runs QgsProcessingAlgorithms.
// §41.2 contract: algorithm outputs go to a TEMP destination; on success the
// result is merged/committed through PaleoProjectStore (temp-then-merge,
// never direct-to-gpkg inside task code).
class QgisProcessingService : public QObject
{
  Q_OBJECT
  public:
    explicit QgisProcessingService(PaleoProjectStore *store, QObject *parent = nullptr);

    // Synchronous run (tests + small tasks). Returns algorithm outputs map.
    QVariantMap run(const QString &algorithmId, const QVariantMap &parameters, QString *error = nullptr);

    // List available paleo:* algorithm ids.
    QStringList paleoAlgorithmIds() const;

  private:
    PaleoProjectStore *m_store;
};
