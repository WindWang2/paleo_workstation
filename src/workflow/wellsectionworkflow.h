// 层：功能
#pragma once
#include "domain/wellsection.h"
#include "services/projectdata.h"
#include "services/seismicmapping.h"
#include <QObject>
#include <QPointer>
#include <QVector>
#include <memory>

class DataCatalog;
class PaleoTask;
class PaleoTaskService;
class SectionWorkbench;
namespace seismic {
class SeismicTaskService;
class SgyVolume;
}

// workflow/ — 连井剖面取数编排：GUI 线程做 catalog 计划步（井/分层/时深/
// 曲线索引），LAS 数据体经 LasCache 在任务线程装载；井间地震缝走
// SeismicTaskService 逐缝提取。不持部件、不画像素。
class WellSectionWorkflow : public QObject {
  Q_OBJECT
public:
  struct WellChoice {
    QString id, name;
    bool hasCoordinates = false;
    double x = qQNaN(), y = qQNaN(); // 井位（视图排序用）
  };
  struct SeismicSource {
    std::shared_ptr<const seismic::SgyVolume> volume;
    SurveyGridGeometry grid;
    double timeOriginMs = 0.0; // 剖面起始时间（catalog survey startTimeMs）
  };
  explicit WellSectionWorkflow(DataCatalog *catalog, QObject *parent = nullptr);
  ~WellSectionWorkflow() override; // 取消在途任务
  void setTaskService(PaleoTaskService *svc); // null → 同步构建
  void setSeismicTaskService(seismic::SeismicTaskService *svc);
  void setSectionWorkbench(SectionWorkbench *wb); // 可选校正；null → 仅 TD 表
  QVector<WellChoice> wellChoices() const;        // wells() 顺序
  // 各井曲线并集（WellCurveRef::mnemonic），大小写不敏感去重（留首个拼写）
  // 并按大小写不敏感排序。
  QStringList availableMnemonics(const QStringList &wellIds) const;
  int request(const QStringList &wellIds, const QStringList &mnemonics); // 返回世代号
  int requestSeismic(const QVector<wellsection::Well> &wells,
                     const SeismicSource &source);
  void cancel();
signals:
  void sectionReady(int generation, const QVector<wellsection::Well> &wells,
                    const QStringList &warnings);
  void seismicReady(int generation, const wellsection::SeismicStrip &strip);

private:
  struct Shared;
  void syncData() const;
  QString projectDir() const;
  static void loadCurveBodies(Shared &shared, PaleoTask *task);

  QPointer<DataCatalog> m_catalog;
  mutable ProjectDataFacade m_data;
  QPointer<PaleoTaskService> m_tasks;
  QPointer<seismic::SeismicTaskService> m_seismicTasks;
  QPointer<SectionWorkbench> m_workbench;
  int m_generation = 0;
  int m_seismicGeneration = 0;
  QPointer<PaleoTask> m_sectionTask;
  QVector<QPointer<PaleoTask>> m_gapTasks;
};
