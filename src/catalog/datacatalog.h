#pragma once
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

// catalog/ — project_area 数据底座（docs/PROJECT_AREA_PLAN.md §3）。
// 对象链：实体 → 显式关联(entity_asset_links) → 数据资产 → 不可变版本。
// catalog.json 是本阶段唯一主存储和查询源（ADR 0056 的 C++ 最小面；
// catalog.sqlite 按计划递延，TODOS P3）。关系从不由标签推断——链接显式落表。
//
// 实体类型：well（稳定 id+井名+UWI+别名）、seismic_survey（打开时从道头冻结
// 角点/inline/crossline 范围/采样间隔/起始时间）、sequence_boundary（层序界面）、
// auxiliary（扫描图、文档、未配准 GeoJSON 等辅助资料）。

struct CatalogEntity
{
  QString id;
  QString entityType;   // "well" | "seismic_survey" | "sequence_boundary" | "auxiliary"
  QString name;
  QString uwi;          // wells only
  QStringList aliases;

  // §3 井坐标：surface_x/y 是原始坐标（本工区=局部测网米），在真投影参数出现前
  // 地图一直读它；project_x/y 仅 coordinate_status=="ok" 时才写。
  double surfaceX = 0.0, surfaceY = 0.0;
  bool hasSurface = false;
  double kb = 0.0, td = 0.0;
  QString coordinateStatus; // "ok" | "untransformed" | "invalid" | "missing"

  // seismic_survey 几何（打开时冻结）。
  double inlineMin = 0, inlineMax = 0, xlineMin = 0, xlineMax = 0;
  double sampleIntervalUs = 0.0, startTimeMs = 0.0;
  QVector<QPair<double, double>> corners; // survey 角点 (x,y)

  QVariantMap extra;    // 前向扩展（如图例字典等）按原样 round-trip
};

struct EntityAssetLink
{
  QString entityType;
  QString entityId;     // unresolved 链接允许为空（§3：资产保留、实体留空、不新建不合并）
  QString assetId;
  QString role;         // well_head | well_log | tops | time_depth | horizon | seismic_volume | reference
  bool isPrimary = true;
  bool unresolved = false;
  QString note;         // 未决备注：双候选记两个规范化井名；零匹配记未匹配名
};

struct CatalogAsset
{
  QString id;
  QString type;         // 分类器类型（well_log / horizon / seismic / ...）
  QString format;       // 小写扩展名
  QString displayName;  // 原始文件 basename
};

struct CatalogVersion
{
  QString id;
  QString assetId;
  QString stage;        // "RAW" | "DERIVED" | "INTERMEDIATE" | "OUTPUT"
  int versionNumber = 1;
  bool managed = true;  // false = 外部链接（SEG-Y 966MB 走这条）
  QString path;         // 受管：工程相对 {stage}/{asset_id}/{version_id}/{filename}；外链：绝对路径
  QString sourceUri;    // 导入源
  QString sha256;       // 受管副本校验和；外链为空
  QString fileName;
  QStringList parentVersionIds; // DERIVED → 源 RAW 版本
  QVariantMap extra;    // 如装箱栅格的碰撞计数
};

class DataCatalog : public QObject
{
  Q_OBJECT
  public:
    explicit DataCatalog(QObject *parent = nullptr);

    // 打开（或初始化）<projectDir>/artifacts/metadata/catalog.json。
    bool open(const QString &projectDir, QString *error = nullptr);
    QString catalogPath() const { return m_dir + QStringLiteral("/artifacts/metadata/catalog.json"); }
    int catalogRevision() const { return m_revision; }

    // 变更：每次落盘（原子写：temp + rename），revision 单调递增。
    bool addEntity(const CatalogEntity &e, QString *error = nullptr);
    bool addAsset(const CatalogAsset &a, QString *error = nullptr);
    bool addVersion(const CatalogVersion &v, QString *error = nullptr);
    bool addLink(const EntityAssetLink &l, QString *error = nullptr);

    bool hasEntity(const QString &id) const;
    QVector<CatalogEntity> entities(const QString &entityType = QString()) const;
    CatalogEntity entityById(const QString &id) const;
    QVector<CatalogAsset> assets() const;
    CatalogAsset assetById(const QString &id) const;
    QVector<CatalogVersion> versionsForAsset(const QString &assetId) const;
    CatalogVersion versionById(const QString &id) const;
    CatalogVersion currentVersion(const QString &assetId) const; // 最高 versionNumber
    QVector<EntityAssetLink> linksForEntity(const QString &entityId) const;
    QVector<EntityAssetLink> linksForAsset(const QString &assetId) const;
    QVector<EntityAssetLink> links() const;

    // 身份解析 §3：井名比较前去首尾空白、连字符、空格，忽略大小写。
    // 返回按此规范化后命中的全部井 id——0/1/2+ 个候选由调用方分别处置
    // （恰好一个挂接；零个再走文件名主名；仍零或两个→unresolved，实体 id 留空、
    // 备注记名，不建井不并井）。
    static QString normalizeWellName(const QString &name);
    QStringList wellsMatchingName(const QString &name) const;

    // 受管路径 {stage}/{asset_id}/{version_id}/{filename}。
    static QString managedPath(const QString &stage, const QString &assetId,
                               const QString &versionId, const QString &fileName);

    // id 分配（单调）：受 "ast-N" / "ver-N"；带前缀实体序号（"aux-3"）。
    QString nextAssetId();
    QString nextVersionId();
    QString nextEntityId(const QString &prefix);

    // 局部测网 CRS（§3）：工作坐标=局部直角米。自定义工程坐标，
    // authid 不落 EPSG:4326（4326 只留作标签）。PROJ 9 的 eqc 必须带椭球，
    // 计划字面串 "+proj=eqc +units=m +no_defs" 补 +ellps=WGS84（本地网格，
    // 椭球不影响米制工作坐标）。
    static QString localGridCrsProj()
    { return QStringLiteral("+proj=eqc +ellps=WGS84 +units=m +no_defs"); }

  signals:
    void changed();      // 任一变更落盘后发射（UI 刷新资产表用）

  private:
    bool save(QString *error = nullptr);
    QString m_dir;
    int m_revision = 0;
    QVector<CatalogEntity> m_entities;
    QVector<CatalogAsset> m_assets;
    QVector<CatalogVersion> m_versions;
    QVector<EntityAssetLink> m_links;
    int m_assetSeq = 0, m_versionSeq = 0;
};
