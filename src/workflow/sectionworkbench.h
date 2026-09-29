// 层：功能
#pragma once
#include "catalog/datacatalog.h"
#include "domain/seismic/sectionwellprojector.h"
#include "io/lasdoc.h"
#include "services/projectdata.h"
#include <QObject>
#include <QPointer>
#include <QVariantList>

// Project-bound section interpretation. UI supplies intentions; this layer
// reads, calibrates and versions well data without holding widgets.
class SectionWorkbench : public QObject {
  Q_OBJECT
public:
  explicit SectionWorkbench(DataCatalog *catalog, QObject *parent = nullptr);
  QVariantList wells();
  QVariantList savedSections() const;
  QVariantMap calibration(const QString &wellId) const;
  bool setCalibration(const QString &wellId, bool constant, double velocity,
                      double shiftMs, QString *error);
  std::vector<seismic::SectionWellInfo> sectionWells();
  std::vector<glm::dvec2> wellRoute(const QStringList &ids, QString *error);
  bool save(const QString &name, const std::vector<glm::dvec2> &route,
            const QString &seismicPath, const QString &horizon, QString *error);
  QVariantMap restore(const QString &versionId, QString *error,
                      const QString &activeSeismicPath = QString());

private:
  void syncProject();
  seismic::TimeDepthModel modelFor(const QString &id, bool md, bool *ok) const;
  QString projectDir() const;
  QPointer<DataCatalog> m_catalog;
  ProjectDataFacade m_data;
  QString m_catalogPath, m_parentVersion;
  QVariantMap m_calibrations;
  QHash<QString, LasDoc> m_logs;
};
