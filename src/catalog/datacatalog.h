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
  QString sha256;       // 受管副本校验和；外链 = 入库时源文件的 SHA-256（§3：打开时校验）
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

    // 把 links() 序中第 index 条未决链接挂到 entityId：置已决、清备注、成为主关联。
    // 同一 (entityType, entityId, role) 下的其他主关联同时降级——§3：同一角色
    // 只保留一条主关联。index 越界 / 链接已决 / entityId 为空 → false。
    bool attachLink(int index, const QString &entityId, QString *error = nullptr);

    // SHA-256 已在库（dedup，§3）：返回第一个匹配版本；sha 为空或无匹配回空版本。
    CatalogVersion versionBySha256(const QString &sha256) const;

    // 外链版本打开校验（§3）：managed==false 且入库时留有 sha256 的版本，
    // 流式重算源文件摘要比对。不一致 → error=「源文件与入库时的 SHA-256 不一致」，
    // 调用方不得解码。受管版本或未留底的外链不校验（恒 true）。
    bool verifyExternalVersionSha(const CatalogVersion &version, QString *error = nullptr) const;

    // 流式计算文件 SHA-256（导入与外链校验共用）；失败回空串 + error。
    static QString sha256FileHex(const QString &path, QString *error = nullptr);

    // 受管路径段合法性（§3）：非空、不是 "." 或含 ".."、不含 /、\\、NUL 与
    // 其他控制字符。任一不满足即非法段。
    static bool isSafePathSegment(const QString &segment);

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
    // 任一段不是合法路径段（isSafePathSegment）时回空串——catalog 不产出坏路径。
    static QString managedPath(const QString &stage, const QString &assetId,
                               const QString &versionId, const QString &fileName);

    // id 分配（单调）：受 "ast-N" / "ver-N"；带前缀实体序号（"aux-3"）。
    QString nextAssetId();
    QString nextVersionId();
    QString nextEntityId(const QString &prefix);

    // 局部测网 CRS（§3 / PROJECT_AREA_PLAN autoplan-eng）：工作坐标=局部直角米。
    // WKT2 ENGCRS：EDATUM 是工程基准，不带大地基准——因此 authid 为空、
    // isGeographic 为假、mapUnits 为米，且不存在到 EPSG:4326 的
    // QgsCoordinateTransform（PROJ 对无基准 CRS 造不出坐标操作，transform
    // isValid()==false）。禁用任何 +proj=eqc：eqc 必须带椭球，椭球意味着
    // 大地基准，会把局部米反投成经纬度。4326 只留在源标签上。
    static QString localGridCrsWkt()
    {
      return QStringLiteral(
          "ENGCRS[\"Paleo local engineering grid\","
          "EDATUM[\"Local engineering datum\"],"
          "CS[Cartesian,2],"
          "AXIS[\"easting\",east,ORDER[1],LENGTHUNIT[\"metre\",1,ID[\"EPSG\",9001]]],"
          "AXIS[\"northing\",north,ORDER[2],LENGTHUNIT[\"metre\",1,ID[\"EPSG\",9001]]]]");
    }

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
