// 层：功能
#pragma once

#include "algorithms/faciesmapping/candidateboundaries.h"
#include "algorithms/faciesmapping/dominantfacies.h"
#include "algorithms/faciesmapping/evidencesynthesis.h"
#include "algorithms/faciesmapping/faciesqa.h"
#include "services/jobrunner.h"

#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <vector>

class DataCatalog;
class QgisLayerService;
class ConstraintStore;

// faciesmappingworkflow — 沉积相自动编图辅助链编排（goal/facies-automapping
// 阶段3/5）。链路：优势相统计（阶段1）→ 候选相界提取（阶段2，GEOS）→
// 多源证据合成（阶段3，权重持久化进 catalog 版本 extra）→ QA 检测（阶段4）
// → 草稿相图 GPKG + QA 报告 JSON 双 DERIVED 资产 + declare「facies_draft.<h>」
// 进 05_PaleoMap（organizer 按层位归组）。三段式协议与 PropertyModelWorkflow
// 同构：compute 纯数据（不碰 catalog/UI），commit 在 owner 线程登记。
// 全部产物走 catalog 版本 + declare/instantiate 正常图层管线，不产游离图层。
class FaciesMappingWorkflow : public QObject
{
  Q_OBJECT
  public:
    explicit FaciesMappingWorkflow( DataCatalog *catalog, const QString &projectDir,
                                    QObject *parent = nullptr );
    void rebind( DataCatalog *catalog, const QString &projectDir );
    void attachLayerService( QgisLayerService *layers );
    // 约束库（随工程打开注入；空 = 装配路径诚实失败，不造数据）。
    void setConstraintStore( ConstraintStore *store );

    // ---- 输入快照（owner 线程构建；纯数据，worker 线程只读） ----
    struct SamplePoint
    {
      double x = 0;
      double y = 0;
      int faciesCode = -1;
      double confidence = 1.0;
    };
    struct SampleSourceInput
    {
      QString id;   // "factor:sandthick" / "remote_prediction" / …
      QString kind; // "factor"|"prediction"（诊断/面板分组用）
      double weight = 1.0;
      std::vector<SamplePoint> points;
    };
    struct HardLineInput
    {
      QString id;
      std::vector<paleo::singlefactor::Point2> points;
    };
    struct ZoneInput
    {
      QString id;
      int faciesCode = -1;
      std::vector<paleo::singlefactor::Polygon> polygons;
    };

    struct DraftFaciesRequest
    {
      QString horizon;
      QString crsWkt; // 产物 GPKG 的 CRS（空 = 无 CRS 直角坐标）

      // 阶段1 输入：井相柱（层位×井区间观测）。
      std::vector<paleo::faciesmapping::WellFaciesColumn> wellColumns;
      // 阶段2 输入：编图域 + 等值线（field_contours 语义）+ 约束相区。
      std::vector<paleo::singlefactor::Polygon> domain;
      std::vector<paleo::singlefactor::ContourLevelLines> contours;
      std::vector<ZoneInput> zones;
      // 阶段4 输入：硬约束线（单元边界不得穿越）。
      std::vector<HardLineInput> hardLines;
      // 阶段3 附加点证据源（井相点票由阶段1 优势相行派生，不在这里重复给）。
      std::vector<SampleSourceInput> extraSources;

      // 证据合成权重（持久化进 catalog 版本 extra = job params）。
      double wellWeight = 1.0;
      double factorWeight = 1.0;
      double predictionWeight = 1.0;
      // 阈值（同上持久化）。
      double assignThreshold = 0.5;
      double minRegionArea = 0;       // 阶段2 碎片丢弃
      double minIslandArea = 0;       // 阶段4 孤岛阈值（<=0 关闭）
      double wellCoverageRadius = 0;  // 阶段4 缺井覆盖缓冲半径
      paleo::faciesmapping::DominantFaciesOptions dominantOptions;
    };

    // 从约束库给请求装配相区（type=polygon 且 facies_code>=0）、硬约束线
    //（type=break_line）与编图域（请求未给域时 = 相区并集）。等值线与井相柱
    // 没有约束库形态，仍由调用方显式给——不在这里伪造。
    bool assembleConstraintInputs( DraftFaciesRequest *request, QString *error = nullptr ) const;

    // ---- compute 段产物（纯数据，GPKG/JSON 序列化字段在 commit 段落位） ----
    struct DraftFaciesComputed
    {
      bool ok = false;
      QString error;
      std::vector<paleo::faciesmapping::CandidateRegion> regions;
      std::vector<paleo::faciesmapping::RegionSynthesis> synthesis;
      std::vector<paleo::faciesmapping::FaciesQaIssue> qaIssues;
      QVariantMap qaDiagnostics;
      QVariantMap dominantDiagnostics;
      QVariantMap boundaryDiagnostics;
      std::vector<paleo::faciesmapping::DominantFaciesRow> dominantRows;
      QVariantMap extra; // catalog 版本 extra（权重/阈值/param_hash/计数）
      QStringList parentPaths;
      int wellVoters = 0;
    };

    struct DraftFaciesJob
    {
      DraftFaciesRequest request;
      DraftFaciesComputed computed;
    };

    // 同步直跑（无任务池的壳/测试）；内部仍是 compute→commit 两段。
    bool run( const DraftFaciesRequest &request, QString *error = nullptr );

    // 纯数据计算（不读成员；#235：worker 线程可安全直调/作静态回调）。
    static DraftFaciesComputed runCompute(
      const DraftFaciesRequest &request,
      const std::function<bool( double, const QString & )> &progress = {} );
    // owner 线程：登记双资产 + declare + stamp；成功 emit draftReady。
    bool commitComputed( const DraftFaciesRequest &request, DraftFaciesComputed *computed );

    // JobRunner 三段式（#235：compute 静态、不捕获 this——对齐
    // PropertyModelWorkflow 的 #163 形态；进度走框架 ProgressFn →
    // PaleoTask::changed，UI 侧接 changed 读 stagePercent()/stage()）。
    PaleoTask *startJob( paleo::jobs::JobRunner<DraftFaciesJob> &runner,
                         const DraftFaciesRequest &request,
                         std::shared_ptr<DraftFaciesJob> *started = nullptr );

  signals:
    void draftReady( const QString &horizon, const QString &layerId );
    void qaReportReady( const QString &reportPath, int issueCount );
    void draftFailed( const QString &reason );

  private:
    QPointer<DataCatalog> m_catalog;
    QPointer<QgisLayerService> m_layers;
    ConstraintStore *m_constraints = nullptr; // 非 QObject：裸指针，注入方保证生命周期
    QString m_projectDir;
};
