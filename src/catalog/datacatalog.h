// 层：数据
#pragma once
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include "roleregistry.h"

#include "catalogindex.h" // D5 邻接索引（纯派生加速结构，四张 QVector 仍是唯一事实源）

// catalog/ — project_area 数据底座（docs/PROJECT_AREA_PLAN.md §3）。
// 对象链：实体 → 显式关联(entity_asset_links) → 数据资产 → 不可变版本。
// catalog.json 是本阶段唯一主存储和查询源（ADR 0056 的 C++ 最小面；
// catalog.sqlite 按计划递延，TODOS P3）。关系从不由标签推断——链接显式落表。
//
// 实体类型：well（稳定 id+井名）、seismic_survey（打开时从道头冻结
// 角点/inline/crossline 范围/采样间隔/起始时间）、sequence_boundary（层序界面）、
// auxiliary（扫描图、文档、未配准 GeoJSON 等辅助资料）。
// D12（pass-2）：uwi/aliases 遗留字段已剥离——井身份只走 name；旧 catalog
// 里的 "uwi"/"aliases" 键装载时静默忽略，不回写。

struct CatalogEntity
{
  QString id;
  QString entityType;   // "well" | "seismic_survey" | "sequence_boundary" | "auxiliary"
  QString name;

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
  int ordinal = 0;      // B 包：同 (entity,role) 成员内业务序（如多 LAS 加载顺序）
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
    // 主文件解析失败且存在 .bak 时回退读 .bak（腐败恢复，只对「解析失败」
    // 生效——schema 版本不匹配是「未来版本」信号，回退旧代数据等于静默
    // 降级，不进该路径）；恢复成功后 recoveredFromBackup() 为真、
    // lastBackupRecoveryReason() 带主文件损坏原因，open 仍算成功。
    bool open(const QString &projectDir, QString *error = nullptr);
    bool isOpen() const { return m_isOpen; }
    // 拒绝写入态：open() 失败、从未成功 open 过、或被锁降级只读 → true。
    // mutator 一律 return false + error，不落盘不改内存——绝不让空 catalog
    // 覆盖坏文件（audit row 36，§9 回滚语义）。
    bool refusesWrites() const { return !m_isOpen || m_lockedReadOnly; }
    QString openError() const { return m_openError; } // open() 失败原因（无则空）
    QString catalogPath() const { return m_dir + QStringLiteral("/artifacts/metadata/catalog.json"); }

    // ---- 单写实例降级（SCHEMA_MIGRATION.md §6：锁失败 = 真只读）----
    // 工程目录被另一实例持锁时由组装根置 true：save() 恒 false（锁错误文
    // 案），mutator 因 save 失败回滚内存；读查询不受影响。open() 不清除该
    // 标志——它是实例级模式，由拥有者（AppContext）管理生命周期。
    void setLockedReadOnly(bool readOnly) { m_lockedReadOnly = readOnly; }
    bool isLockedReadOnly() const { return m_lockedReadOnly; }

    // ---- 腐败恢复面（open() 的 .bak 回退结果）----
    bool recoveredFromBackup() const { return m_recoveredFromBackup; }
    QString lastBackupRecoveryReason() const { return m_backupRecoveryReason; }
    int catalogRevision() const { return m_revision; }

    // 工程角色词表（DATA_FABRIC_ADOPTION A 包）：open() 时读
    // <projectDir>/project_area.json 的 roles 节覆盖内置词表；缺文件/坏 JSON
    // → defaults()，永不计入 open 失败。addLink/attachLink 收 role 时经
    // isKnown 校验——诚实降级而非硬拦（词表是工程自定义的，project_area.json
    // 可扩；硬拦会把合法自定义挡在旧二进制外），见 invalidRoleLinks()。
    const RoleRegistry &roleRegistry() const { return m_roles; }

    // ---- wave/io-perf-cache D5：索引化查询 + 计数缓存 + 备份轮转 ----
    // 类型计数缓存（D5.3）：邻接索引维护，不再每次全量算。
    QHash<QString, int> entityCountsByType() const;
    // 索引健康对账（诊断/测试面：索引 vs 四表线性扫描）。
    bool indexHealthy(QString *mismatch = nullptr) const;
    // 备份轮转保留代数（D5.6）：catalog.bak（最新一代）+ .bak.2 … .bak.N；
    // 默认 3，范围 [1,9]。open() 的 .bak 回退永远读最新一代。
    int backupKeepCount() const { return m_backupKeep; }
    void setBackupKeepCount(int generations) { m_backupKeep = qBound(1, generations, 9); }

    // 词表违例链接集（诊断面）：addLink/attachLink 写入的「未知角色」与
    // 「角色与实体类型不符」链接——扫描 note 诊断标记，随 catalog.json
    // round-trip，重开后仍可查。词表内的链接不在其中。
    QVector<EntityAssetLink> invalidRoleLinks() const;

    // 批量写作用域（audit row 37 / T33）：构造期间 mutator 只做校验+内存变更，
    // save() 被挂起；析构（或显式 flush）落一次盘、发一次 changed()。
    // 文件夹导入 ~5N 次全量序列化因此收敛成一次；中途崩溃不留「资产入库但
    // 链接没落盘」的半截状态。嵌套安全（深度计数）。析构 flush 失败只记
    // qWarning——需要错误面的调用方请自己调 flush()。
    class BatchSave
    {
      public:
        explicit BatchSave(DataCatalog *catalog);
        ~BatchSave();
        BatchSave(const BatchSave &) = delete;
        BatchSave &operator=(const BatchSave &) = delete;
        bool flush(QString *error = nullptr); // 立即结算一次；幂等
      private:
        DataCatalog *m_catalog = nullptr;
        bool m_done = false;
    };

    // 变更：每次落盘（原子写：temp + rename），revision 单调递增。
    bool addEntity(const CatalogEntity &e, QString *error = nullptr);
    bool addAsset(const CatalogAsset &a, QString *error = nullptr);
    bool addVersion(const CatalogVersion &v, QString *error = nullptr);
    bool addLink(const EntityAssetLink &l, QString *error = nullptr);

    // 把 links() 序中第 index 条未决链接挂到 entityId：置已决、清备注、成为主关联。
    // 同一 (entityType, entityId, role) 下的其他主关联同时降级——§3：同一角色
    // 只保留一条主关联。index 越界 / 链接已决 / entityId 为空 → false。
    bool attachLink(int index, const QString &entityId, QString *error = nullptr);

    // attachLink 的撤销面（资产表「未决」行的会话内回退，§4）：links() 序第
    // index 条已决链接改回未决——entityId 清空、unresolved=true、isPrimary 降级、
    // 备注清空。资产与被共享的井实体都保留（未决不是删除）。index 越界或链接
    // 本来就是未决 → false。
    bool setLinkUnresolved(int index, QString *error = nullptr);

    // 「将此版本设为主版本」（§4）：links() 序第 index 条已决链接提升为同角色
    // 主关联——本链接 isPrimary=true，同一 (entityType, entityId, role) 下的
    // 其他主关联降级（与 addLink/attachLink 同一不变量）。只动链接标志，
    // 不复制版本字节。index 越界 / 链接未决或实体 id 为空 → false。
    bool setLinkPrimary(int index, QString *error = nullptr);

    // SHA-256 已在库（dedup，§3）：返回第一个匹配版本；sha 为空或无匹配回空版本。
    CatalogVersion versionBySha256(const QString &sha256) const;
    // Returns an empty path for an unsafe managed path, including symlinked
    // ancestors. External links retain their absolute source path.
    static QString resolvedVersionPath(const QString &projectDir, const CatalogVersion &version);

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
    // 全量版本表（只读快照口径——IngestPlan 构建 worker 线程用；COW O(1)）。
    QVector<CatalogVersion> versions() const { return m_versions; }
    CatalogVersion versionById(const QString &id) const;
    CatalogVersion currentVersion(const QString &assetId) const; // 最高 versionNumber
    // 空 entityId 是调用方 bug（audit row 35）：如实返回空集，不再静默命中
    // 全部未决链接。要未决集合请用 unresolvedLinks()。
    QVector<EntityAssetLink> linksForEntity(const QString &entityId) const;
    QVector<EntityAssetLink> linksForAsset(const QString &assetId) const;
    QVector<EntityAssetLink> links() const;
    QVector<EntityAssetLink> unresolvedLinks() const; // unresolved==true 的全部链接

    // ---- B 包：下游闭包与 staleness-lite（DATA_FABRIC_ADOPTION；上游
    // impact/entity_views 的裁剪面） ----

    // parentVersionIds 反查全闭包：versionId 的全部下游版本（BFS，环安全，
    // 不含种子本身）。空 id → 空集；未知 id 如实回空集（下游按链接表存在性
    // 判定，种子是否在库不影响边扫描）。
    QVector<CatalogVersion> downstreamClosure(const QString &versionId) const;

    // 下游失效标记（staleness-lite）：versionId 下游闭包中的 DERIVED 版本
    // 记 extra["stale"]=true + extra["staleReason"]=reason（reason 空 →
    // 通用原因）。供预览/校验路径在外链 verifyExternalVersionSha 失配后调用——
    // 「父版本源字节变了 ⇒ 下游产物过期」。空/未知 versionId → false+error；
    // 无下游或标记未变 → true 不落盘（不空涨 revision）。
    bool markDownstreamStale(const QString &versionId, const QString &reason,
                             QString *error = nullptr);

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

    // 井位快照（§4 地图高亮）：实体表里有 surface 坐标的井写成点要素
    // GeoJSON（properties: id/name/coordinate_status，legacy "crs" 成员写
    // 工程米制 WKT——OGR 认它，不落到 4326）。没有可定位的井时不写文件、
    // 返回 true（与 wells.thickness 同一约定）；写盘失败 → false + error。
    bool writeWellsGeoJson(const QString &path, QString *error = nullptr) const;

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
    // open() 经 .bak 回退恢复成功后发射一次（主文件损坏原因随行）——UI/
    // 状态面据此向用户告警「catalog 已从备份恢复，主文件损坏」。
    void backupRecovered(const QString &reason);

  private:
    bool ensureOpen(QString *error) const;
    bool save(QString *error = nullptr);
    void beginBatch();
    bool endBatch(QString *error = nullptr);
    // markDownstreamStale/addVersion 共用的内存段标记：只写 m_versions，
    // 不落盘；返回实际改动的版本数（已是同一标记的不计）。
    // undo 非空时，每个被改行在改前压入 (行号, 原版本拷贝)——调用方在
    // save 失败回滚时逆序还原（同一行可能重复入栈，逆序恢复到最初原值）。
    // 行号定位走 m_idx.versionRow（O(1)）——WP2：不再每次全表建 id→row 哈希
    //（那是 addVersion 的隐藏 O(N)/调用，N 次导入即 O(N²)）。
    int markStaleDownstreamOf(const QString &versionId, const QString &reason,
                              QVector<QPair<int, CatalogVersion>> *undo = nullptr);
    QString m_dir;
    bool m_isOpen = false;
    QString m_openError;         // 最近一次 open() 失败原因（成功后清空）
    bool m_lockedReadOnly = false; // 见 setLockedReadOnly——实例级只读降级
    bool m_recoveredFromBackup = false; // open() 走了 .bak 回退（本次 open 内）
    QString m_backupRecoveryReason;     // 主文件损坏原因（恢复成功时留底）
    bool m_primaryCorruptOnDisk = false; // 盘上主文件仍是损坏那份（首次成功 save 前，#79）
    int m_batchDepth = 0;        // >0 时 save() 挂起（BatchSave）
    bool m_batchDirty = false;   // 挂起期间有过变更 → endBatch 落一次盘
    int m_revision = 0;
    QVector<CatalogEntity> m_entities;
    QVector<CatalogAsset> m_assets;
    QVector<CatalogVersion> m_versions;
    QVector<EntityAssetLink> m_links;
    int m_assetSeq = 0, m_versionSeq = 0;
    RoleRegistry m_roles;            // 见 roleRegistry()——open() 时装载
    CatalogIndex m_idx;              // D5 邻接索引（随 mutator 增量维护）
    int m_backupKeep = 3;            // D5.6 备份轮转代数
};
