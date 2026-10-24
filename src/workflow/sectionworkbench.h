// 层：功能
#pragma once
#include "catalog/datacatalog.h"
#include "domain/seismic/sectionwellprojector.h"
#include "domain/wellsection.h" // wellsection::Well（wellSnapWells 吸附井集）
#include "io/lasdoc.h"
#include "services/projectdata.h"
#include <QObject>
#include <QPointer>
#include <QVariantList>
#include <atomic>
#include <functional>
#include <memory>

class PaleoTaskService;
// Project-bound section interpretation. UI supplies intentions; this layer
// reads, calibrates and versions well data without holding widgets.
class SectionWorkbench : public QObject {
  Q_OBJECT
public:
  ~SectionWorkbench() override;
  void setTaskService(PaleoTaskService *tasks) { m_tasks = tasks; }
  explicit SectionWorkbench(DataCatalog *catalog, QObject *parent = nullptr);
  QVariantList wells(const std::shared_ptr<std::atomic_bool> &stop = {});
  QVariantList savedSections() const;
  QVariantMap calibration(const QString &wellId) const;
  bool setCalibration(const QString &wellId, bool constant, double velocity,
                      double shiftMs, QString *error);
  std::vector<seismic::SectionWellInfo> sectionWells(
      const std::shared_ptr<std::atomic_bool> &stop = {});
  struct PreviewData {
    QVariantList wells;
    std::vector<seismic::SectionWellInfo> sectionWells;
  };
  void requestPreviewData(std::function<void(const PreviewData &)> ready);
  void cancelPreviewData();
  // MD 域时深：逐井校正优先（常速校正 → 常速模型），否则主时深表 MD 列严格表；
  // shift 为校正时间平移。false = 无可用时深，status 写原因
  // （「无时深表」/「时深表无序或有效样点不足」）。
  bool mdTimeDepth(const QString &wellId, seismic::TimeDepthModel *model,
                   double *shiftMs, QString *status);
  QString wellForSource(const QString &path, const QString &name);
  std::vector<glm::dvec2> wellRoute(const QStringList &ids, QString *error);
  // 「按空间关系重排」：mode "nearest"=最近邻链 / "pca"=主轴投影（domain
  // orderWellsNearestNeighbor / orderWellsByPosition）；井坐标取工程井实体
  // （coordinateStatus ok/untransformed 且有限，与 wellRoute 同口径）。
  // 未知 mode 原样返回。
  QStringList spatiallyOrderedWells(const QStringList &ids, const QString &mode);
  // 画线吸附井集：有效坐标井（口径同上）的 id/name/x/y，供 SeismicSectionTool
  // 做最近井吸附（wellsection::Well 只填坐标身份字段）。
  QVector<wellsection::Well> wellSnapWells();
  bool save(const QString &name, const std::vector<glm::dvec2> &route,
            const QString &seismicPath, const QString &horizon, QString *error);
  QVariantMap restore(const QString &versionId, QString *error,
                      const QString &activeSeismicPath = QString());

public:
  // PW-035：标定文件读取失败原因（空 = 正常或无文件）。
  QString calibrationLoadError() const { return m_calibrationLoadError; }

signals:
  void alignmentsChanged();

private:
  PaleoTaskService *m_tasks = nullptr;
  void syncProject();
  seismic::TimeDepthModel modelFor(const QString &id, bool md, bool *ok) const;
  QString projectDir() const;
  bool storeCalibrations(const QVariantMap &calibrations, QString *error) const;
  QPointer<DataCatalog> m_catalog;
  ProjectDataFacade m_data;
  QString m_catalogPath, m_parentVersion;
  QString m_calibrationLoadError;
  QVariantMap m_calibrations;
  QHash<QString, LasDoc> m_logs;
  quint64 m_previewGeneration = 0;
  bool m_previewRunning = false, m_previewPending = false;
  std::shared_ptr<std::atomic_bool> m_previewStop;
  std::function<void(const PreviewData &)> m_previewReady;
};
