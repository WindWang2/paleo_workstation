// 层：功能
#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "../algorithms/wellsiting/wellsiting.h"

#include <functional>
#include <vector>

class DataCatalog;
class PaleoProjectStore;
class ProjectDataFacade;
class QgisLayerService;
class WellSitingStore;

// wellsitingworkflow — 井网辅助编排（方向 34：覆盖诊断、候选点位、方案
// 对比、planned 计划井实体）。thin orchestrator：几何计算全在
// algorithms/wellsiting，持久化全在 catalog（planned 实体）与
// WellSitingStore（方案快照），图层声明走 QgisLayerService。
//
// planned 隔离红线：实井输入只认 entityType=="well"（catalog 直查按
// hasSurface 过滤，或 ProjectDataFacade::wells()——两者内部同过滤）；
// planned 实体绝不进入任何计算输入——评估时显式以「实井 + 候选点」拼装。
//
// planned 可见性面：catalog 实体创建后不可变（无 update/remove API），
// 计划井的「删除」= siting 文档 retired 清单、改名 = renames 覆盖——
// catalog 实体保持审计痕迹，siting 查询面（列表/图层/评估/导出）统一过滤。
//
// 诚实面：指标口径随结果 note 如实上屏/落盘（欧氏近似、栅格面积、域回退
// 策略）；覆盖评估不声称地质最优。
struct WellSitingParams
{
  double controlRadius = 2500; // 井控半径（米）
  double cellSize = 100;       // 采样格边长（米）
  double gridSpacing = 0;      // 候选网格间距；0 → controlRadius
  double lineBuffer = 300;     // 候选距约束线缓冲（米）
  double boundaryMargin = 200; // 候选距工区边界内缩（米）
  double minWellDistance = 0;  // 候选距既有井最小距离；0 → 不限
  int perHoleLimit = 0;        // 每空洞网格候选上限；0 → 不限

  QVariantMap toMap() const;
  static WellSitingParams fromMap( const QVariantMap &map );
};

// 避让/域的外部几何（WKT，工程坐标系）。type: "line" | "polygon" | "point"。
struct SitingConstraintGeometry
{
  QString wkt;
  QString type;
};

class WellSitingWorkflow : public QObject
{
  Q_OBJECT
  public:
    explicit WellSitingWorkflow( QgisLayerService *layers, PaleoProjectStore *store,
                                 QObject *parent = nullptr );
    ~WellSitingWorkflow() override;

    // 绑定面（AppContext 装配 / 测试注入子集）。
    void setProjectData( ProjectDataFacade *projectData );
    void setCatalog( DataCatalog *catalog, const QString &projectDir = QString() );
    void setSitingStore( WellSitingStore *store );
    // 约束几何提供方（ConstraintStore 等的外部归集）：第一个 polygon 作为
    // 工区域，line 全部作为候选避让线。缺省（空）→ 域回退井位包围盒。
    void setConstraintProvider( std::function<QList<SitingConstraintGeometry>()> provider );
    // 断层切割多边形提供方（WKT 列表，FaultSetStore 归集，可空）→ 候选禁钻面。
    void setFaultCutsProvider( std::function<QStringList()> wktProvider );

    // —— 覆盖诊断（目标形态 1）——
    // 实井采样 → 空洞报告 → 诊断图层（07_Validation，layerId
    // "wellsiting_holes"）。无 projectDir/图层服务时仍出报告（跳过落图）。
    bool runDiagnosis( const WellSitingParams &params, QString *error = nullptr );
    QVariantMap lastDiagnosisSummary() const { return m_lastDiagnosis; }
    QList<QVariantMap> lastHoles() const { return m_lastHoles; }

    // —— 候选点位（目标形态 2）——
    bool generateCandidates( const WellSitingParams &params, QString *error = nullptr );
    QList<QVariantMap> lastCandidates() const { return m_lastCandidates; }
    void clearCandidates();

    // —— planned 计划井 CRUD（目标形态 4 的数据面）——
    QString addPlannedWell( const QString &name, double x, double y,
                            QString *error = nullptr ); // 返回实体 id
    bool movePlannedWell( const QString &entityId, double x, double y,
                          QString *error = nullptr );
    bool renamePlannedWell( const QString &entityId, const QString &name,
                            QString *error = nullptr );
    bool removePlannedWell( const QString &entityId, QString *error = nullptr );
    QList<QVariantMap> plannedWells() const; // {id,name,x,y}
    // 计划井是否已弃用（siting 文档 retired 面）——数据页树过滤等展示面
    // 与 siting 面保持一致用（不涉及计算输入）。
    bool isPlannedRetired( const QString &entityId ) const;

    // —— 方案评估（目标形态 3）——
    // 基线（实井）vs 方案（实井 + 所列 planned 井）+ 逐井贡献分。
    QVariantMap evaluateScenario( const WellSitingParams &params,
                                  const QStringList &plannedIds,
                                  QString *error = nullptr );
    QVariantMap lastEvaluation() const { return m_lastEvaluation; }

    // —— 方案持久化（目标形态 5）——
    bool saveScenario( const QString &name, const QStringList &plannedIds,
                       const WellSitingParams &params, QString *error = nullptr );
    QList<QVariantMap> scenarios() const; // {id,name,well_count,updated_ms,metrics_*,…}
    bool deleteScenario( const QString &scenarioId, QString *error = nullptr );

    // —— 导出（目标形态 6）——
    bool exportScenarioCsv( const QString &scenarioId, const QString &path,
                            QString *error = nullptr );
    bool exportComparisonChart( const QString &path, QString *error = nullptr );

    // 诊断/评估输入的实井集（测试与隔离断言用；只认 entityType=="well"）。
    QList<QVariantMap> realWellsForSiting() const;

  signals:
    void diagnosisDone( const QVariantMap &summary );
    void candidatesChanged();
    void plannedWellsChanged();
    void evaluationDone( const QVariantMap &summary );
    void scenariosChanged();

  private:
    struct Impl;
    Impl *d = nullptr;

    // 域解析（诊断/候选/评估共用回退链）：polygon 约束 → 地震工区角点 →
    // 井位包围盒外扩 controlRadius。*source 如实带回策略注记。
    std::vector<paleo::wellsiting::Polygon> resolveDomain( const WellSitingParams &params,
                                                           const QList<QVariantMap> &wells,
                                                           QString *source = nullptr ) const;
    // 诊断图层落图（无工程目录/图层服务时静默跳过——报告仍有效）。
    void writeHolesLayer( const paleo::wellsiting::CoverageReport &report );
    // planned_wells 图层（00_Data）刷新。
    void refreshPlannedLayer();
    void retirePlannedWell( const QString &entityId );
    void ensureScenariosLoaded() const;
    bool saveScenarioSet( QString *error ) const;

    QVariantMap m_lastDiagnosis;
    QList<QVariantMap> m_lastHoles;
    QList<QVariantMap> m_lastCandidates;
    QVariantMap m_lastEvaluation;
};
