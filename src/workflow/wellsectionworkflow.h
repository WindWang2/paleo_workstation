// 层：功能
#pragma once
#include "domain/wellsection.h"
#include "services/projectdata.h"
#include "services/seismicmapping.h"
#include <QObject>
#include <QHash>
#include <QPointer>
#include <QVector>
#include <memory>

class DataCatalog;
class FaultSetStore;
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
  // 共用真实解释/岩屑 provider；不装载曲线或图片。
  QHash<QString, QVector<wellsection::LithoSegment>> lithologiesFor(const QStringList &wellIds, QStringList *warnings = nullptr) const;
  // ---- 解释岩性来源仲裁（方向 98：同井多份解释资产显式选择）----
  struct LithoSourceChoice {
    QString assetId;      // 消费指向的资产 id
    QString label;        // displayName（vN）——选择面与题注/告警同名
    int versionNumber = 0;
  };
  // 同井候选（interpretation 角色 + well_litho_intervals 类型），版本号降序
  //（最新在前）。空 = 无解释资产（岩屑兜底/GR 回落是正常态）。
  QVector<LithoSourceChoice> lithoSourceChoices(const QString &wellId) const;
  // 显式选择（assetId 空 = 回落默认「取最新」）。仅记 workflow 内存态——
  // 持久化归壳层（WellSectionStore 解释选择表）；消费时选择不在候选内
  // → 回落默认并如实告警。默认仲裁规则不变（无选择 = 最新版本号）。
  void setLithoAssetSelection(const QString &wellId, const QString &assetId);
  // 整表恢复（开工程/换工程时壳层灌入；清空传空表）。
  void setLithoAssetSelections(const QHash<QString, QString> &selection);
  QHash<QString, QString> lithoAssetSelections() const { return m_lithoSelection; }
  // ---- 分层来源切换（剖面级：同井多份 tops 资产显式选择层序）----
  struct TopsSourceChoice {
    QString assetId;      // 消费指向的资产 id
    QString label;        // displayName（vN）
  };
  // 剖面井集（空 = 全部工程井）tops 链接的并集，去重按资产 id——层序词表
  // 是文件级的，不同井可能挂不同文件（主分层/细分层并存是常态）。
  QVector<TopsSourceChoice> topsSourceChoices(const QStringList &wellIds) const;
  // 空 = 各井主分层链接（默认，旧行为）。选择对未挂接该资产的井不伪造分
  // 层——如实空 tops + 告警点名。
  void setTopsSource(const QString &assetId) { m_topsSource = assetId; }
  QString topsSource() const { return m_topsSource; }
  int request(const QStringList &wellIds, const QStringList &mnemonics); // 返回世代号
  int requestSeismic(const QVector<wellsection::Well> &wells,
                     const SeismicSource &source);
  // 取消在途取数并作废当前世代（两条世代号都 +1）：之后到达的旧结果一律
  // 不等于 currentGeneration()，被接收方丢弃。
  void cancel();
  // ---- 断层投绘（goal/wellsection-deep）----
  void setFaultSetStore(FaultSetStore *store); // 可空 → 状态提示
  // 断面 mesh ∩ 井径 curtain（同步；几何量小）。traces 按井序沿井口连线，
  // status 空 = 正常出线。
  struct FaultProjection {
    QVector<wellsection::FaultTrace> traces;
    QString status;
  };
  FaultProjection faultProjection(const QVector<wellsection::Well> &wells) const;
  // #128：当前世代。接收方按它过滤 sectionReady/seismicReady——不要用
  // request()/requestSeismic() 的返回值：无任务服务或早退路径会在 return
  // 之前就同步 emit，调用方此时还没来得及记下返回值。
  int currentGeneration() const { return m_generation; }
  int currentSeismicGeneration() const { return m_seismicGeneration; }

public:
  // 解释资产解析结果的跨请求缓存条目（按「版本解析路径 + mtime」键控，
  // 内容替换自然失效；仅 GUI 线程的计划步触达，无锁）。.cpp 内的
  // provider 与 attachFaciesSegments 共用（故为 public 嵌套类型）。
  struct InterpParseCacheEntry {
    bool parsed = false; // 是否已解析过（区分「未缓存」与「空结果」）
    int dropped = 0;     // 岩性解析丢弃行数（缓存命中不再重复告警）
    QHash<QString, QVector<wellsection::LithoSegment>> lithoByWell;
    QHash<QString, QVector<wellsection::FaciesSegment>> faciesByWell;
  };

signals:
  void sectionReady(int generation, const QVector<wellsection::Well> &wells,
                    const QStringList &warnings);
  void seismicReady(int generation, const wellsection::SeismicStrip &strip);
  // 解释来源选择变化（wellId + 新 assetId；空 = 回落默认）。壳层接：落库 +
  // 重取剖面。程序化 setLithoAssetSelections（恢复路径）不发本信号。
  void lithoSelectionChanged(const QString &wellId, const QString &assetId);

private:
  struct Shared;
  void syncData() const;
  QString projectDir() const;
  static void loadCurveBodies(Shared &shared, PaleoTask *task);
  void attachFaciesSegments(QVector<wellsection::Well> &wells) const;
  void attachLithoSegments(QVector<wellsection::Well> &wells,
                           QStringList *warnings) const;
  // 图片道清单（GUI 步）：core/lab_analysis 深度锚图片的路径/深度/标注——
  // QImage 装载在任务线程（loadCurveBodies 尾段）。
  void collectCoreImages(Shared &shared) const;

  QPointer<DataCatalog> m_catalog;
  mutable ProjectDataFacade m_data;
  QPointer<PaleoTaskService> m_tasks;
  QPointer<seismic::SeismicTaskService> m_seismicTasks;
  QPointer<SectionWorkbench> m_workbench;
  FaultSetStore *m_faultStore = nullptr; // 拥有方为壳层（非 QObject）
  int m_generation = 0;
  int m_seismicGeneration = 0;
  QPointer<PaleoTask> m_sectionTask;
  QVector<QPointer<PaleoTask>> m_gapTasks;
  // 解释岩性/相 JSON 跨请求解析缓存：换曲线/换井的重复请求不再整文件
  // 重读盘与重复告警（此前 provider 每请求新建，缓存随实例销毁）。
  mutable QHash<QString, InterpParseCacheEntry> m_interpParseCache;
  // 解释岩性显式选择（wellId → assetId；无条目 = 默认取最新）。
  QHash<QString, QString> m_lithoSelection;
  // 分层来源显式选择（剖面级；空 = 各井主 tops 链接）。
  QString m_topsSource;
};
