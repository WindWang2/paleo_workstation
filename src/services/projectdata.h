// 层：数据
#pragma once
#include <QObject>
#include <QString>
#include <QVector>

#include "../catalog/entityview.h"
#include "../domain/deviationsurvey.h"

#include <optional>

class LayerManifest;
struct WellParseReport;

// services/ — ProjectDataFacade: the READ side of the project_area data
// foundation for the mapping pipeline (docs/PROJECT_AREA_PLAN.md §3, 阶段C)。
// Everything the mapping chain needs from the catalog flows through these four
// queries; nothing in the mapping chain parses entity files itself.
//
// Sources
//   · DataCatalog (src/catalog) — catalog.json 的权威读写面，位于
//     <projectDir>/artifacts/metadata/catalog.json。实体（井：surface_x/y +
//     coordinate_status）、显式关联（role、primary、unresolved）、资产与
//     不可变版本（受管相对路径 {stage}/{asset_id}/{version_id}/{filename}）。
//   · 井文件解析（src/io/wellfileparsers）— tops/时深按 SMI 列契约解析，
//     -99999 视为空；这里不另写解析。
//   · LayerManifest — 声明的地图结果图层（§37）。层位 DERIVED 时间栅格
//     （D61 411×641）登记于此；horizonRasterDecl() 解析声明并直接从文件
//     （GDAL）读网格几何。
//
// 接口契约（两包接缝，主版本 wave/mapping-pipeline）：
//   wells()          → type=="well" 实体（surface 坐标 + coordinate_status）
//   topsFor(id)      → 该井主 tops 版本文件里的分层（WellTop.horizon=层名）
//   tdTableFor(id)   → 该井主 time_depth 版本文件（文件顺序，不排序）
//   horizonRasterDecl(h) → LayerManifest raster 声明（优先精确 id
//     "horizon.<h>"，再退 "horizon." 前缀，最后任何绑到该层位的 raster）
struct ProjectWell
{
  QString id;                 // stable entity id, e.g. "well-1"
  QString name;               // display name, e.g. "A1"
  double surfaceX = 0.0;      // local meter grid (原始坐标；untransformed 时一直用它)
  double surfaceY = 0.0;
  double kb = 0.0;            // 补心海拔 m（海平面以上为正；缺数据 = 0）
  QString coordinateStatus;   // ok | untransformed | invalid | missing
};

struct WellTop
{
  QString horizon;            // boundary name as picked, e.g. "D61"
  double md = qQNaN();        // measured depth, meters (NaN = -99999/缺列)
  double tvd = qQNaN();       // true vertical depth, meters
  double x = qQNaN();         // 分层点 X/Y（tops 文件 X/Y 列；NaN = 无坐标）
  double y = qQNaN();
};

struct TdSample
{
  double timeMs = 0.0;        // two-way time, milliseconds
  double tvd = qQNaN();       // true vertical depth, meters (NaN = -99999/缺列)
  double md = qQNaN();        // measured depth, meters (NaN = -99999/缺列)
};

struct HorizonRasterInfo
{
  bool valid = false;
  QString layerId;            // manifest layer id, e.g. "horizon.D61.derived"
  QString path;               // raster file on disk
  int rows = 0, cols = 0;
  double cellSize = 0.0;      // grid cell size, meters (survey bin size)
  double xmin = 0.0, xmax = 0.0, ymin = 0.0, ymax = 0.0;
  int inlineMin = -1, inlineMax = -1; // PALEO_INLINE_MIN/MAX metadata, -1 unknown
};

class ProjectDataFacade : public QObject
{
  Q_OBJECT
  public:
    explicit ProjectDataFacade(QObject *parent = nullptr);
    ~ProjectDataFacade() override;

    // 打开 <projectDir>/artifacts/metadata/catalog.json（数据底座的产物）。
    // False（lastError 置位）当目录里没有 catalog；查询随后返回空。
    bool setProjectDir(const QString &projectDir);

    // 直接绑定 catalog（测试/注入用；不接管所有权）。projectDir 用于解析
    // 受管相对路径；留空则从 catalogPath() 推导。
    void setCatalog(DataCatalog *catalog, const QString &projectDir = QString());

    // LayerManifest supplies horizon raster declarations; not owned.
    void setManifest(LayerManifest *manifest);

    QVector<ProjectWell> wells() const;
    QVector<WellTop> topsFor(const QString &wellId) const;      // empty when unlinked
    // 文件级解析诊断（筛井前）；保留旧入口，报告每次复位。
    QVector<WellTop> topsFor(const QString &wellId, WellParseReport *report) const;
    QVector<TdSample> tdTableFor(const QString &wellId) const;  // empty when unlinked — callers must not fabricate times
    // 主 trajectory 链接（primary、非 unresolved）的测斜站表 → 三维轨迹。
    // 三态（方向 69）：有效站表 → survey；无链接 → nullopt（= 直井，显式
    // 语义不告警：调用方保持原垂直路径，绝不虚构造斜）；文件不可读/站表
    // 无效 → nullopt + 记因。error 出参按本次调用写明（无链接 = 空串；
    // 坏文件/坏表 = 具体原因）——不依赖 lastError 跨调用残留。失败原因
    // 同样记 lastError。
    std::optional<paleo::WellDeviationSurvey> trajectoryFor(
        const QString &wellId, QString *error = nullptr) const;
    HorizonRasterInfo horizonRasterDecl(const QString &horizon) const;

    QString lastError() const { return m_lastError; }

    // EntityView 查询面直通（data/entity-view）：注册表全角色槽 + primary/成员/
    // 未决分桶 + 下游产物 + 悬空源诊断——DataPage 与派生链消费的稳定出口。
    // catalog 未开/实体未知 → 如实空视图（entity.id 为空），不编造槽位。
    EntityView entityView(const QString &entityId) const;
    // 版本下游闭包（parentVersionIds 反查 BFS）：重派生影响面评估用。
    QVector<CatalogVersion> downstreamClosureOf(const QString &versionId) const;

  private:
    // 主关联（primary、非 unresolved）最新版本的绝对路径；空 = 无该角色关联。
    QString assetFilePathFor(const QString &wellId, const QString &role) const;
    QString projectDir() const;

    DataCatalog *m_catalog = nullptr;   // setProjectDir 时为自有子对象
    bool m_ownsCatalog = false;
    QString m_projectDirOverride;
    LayerManifest *m_manifest = nullptr;
    mutable QString m_lastError; // const readers may record lookup failures
};
