// 层：数据
#pragma once
#include <functional>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include "roleregistry.h"

#include "catalogindex.h" // D5 邻接索引（纯派生加速结构，四张 QVector 仍是唯一事实源）

#include <memory>

class CatalogStore;

// catalog/ — project_area 数据底座（docs/PROJECT_AREA_PLAN.md §3）。
// 对象链：实体 → 显式关联(entity_asset_links) → 数据资产 → 不可变版本。
// 查询事实源是内存四表。盘上主库是 catalog.sqlite（CatalogStore）；
// catalog.json 只作迁移输入。关系从不由标签推断——链接显式落表。
//
// 实体类型：well（稳定 id+井名）、seismic_survey（打开时从道头冻结
// 角点/inline/crossline 范围/采样间隔/起始时间）、sequence_boundary（层序界面）、
// auxiliary（扫描图、文档、未配准 GeoJSON 等辅助资料）、
// planned（方向 34 计划井：surface 坐标 + 部署依据；虚拟实体，绝不进入
// 单因素/编图/剖面等计算输入——计算侧一律按 entityType=="well" 过滤）。
// D12（pass-2）：uwi/aliases 遗留字段已剥离——井身份只走 name；旧 catalog
// 里的 "uwi"/"aliases" 键装载时静默忽略，不回写。

struct CatalogEntity
{
  QString id;
  QString entityType;   // "well" | "seismic_survey" | "sequence_boundary" | "auxiliary" | "planned"
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
  QString role;         // 链接角色——分组/树排序序/显示名的唯一权威是
                        // catalogroles.h 的 catalogRoles()（方向 92 单源：
                        // 树角色序与 AI 工具描述共用，新增角色先入表）
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

// 审计 02 M-8 produce-then-commit：catalog 变更日志项。staging 副本（见
// DataCatalog::createStagingCopy）上每个成功的 mutator 记一条；owner 线程
// applyJournal 按原序用同一批 mutator 重放（同一基线 mutationSeq ⇒ 同一结果）。
// 纯值类型——跨线程交接安全。
struct CatalogOp
{
  enum class Kind
  {
    AddEntity,
    AddAsset,
    AddVersion,
    AddLink,
    AttachLink,
    SetLinkUnresolved,
    SetLinkPrimary,
    MarkDownstreamStale,
    UpdateVersionExtra // 方向 79：版本 extra 就地更新（锚深后补编辑）
  };
  Kind kind = Kind::AddAsset;
  CatalogEntity entity;
  CatalogAsset asset;
  CatalogVersion version;
  EntityAssetLink link;
  int index = -1;   // Attach/SetLink*：links() 序号（基线 mutationSeq 下有效）
  QString id;       // AttachLink 实体 id；MarkDownstreamStale/UpdateVersionExtra 版本 id
  QString reason;   // MarkDownstreamStale
  QString extraKey; // UpdateVersionExtra：extra 键
  QVariant extraValue; // UpdateVersionExtra：新值（无效 QVariant = 删键）
};

class DataCatalog : public QObject
{
  Q_OBJECT
  public:
    explicit DataCatalog(QObject *parent = nullptr);
    ~DataCatalog() override;

    // 打开（或初始化）工程目录上的 catalog。盘上主库是
    // artifacts/metadata/catalog.sqlite；catalog.json 只作迁移输入。
    // 主库损坏且存在 sqlite .bak 时回退（schema / user_version 过新是未来
    // 版本信号，不回退旧代）。恢复成功后 recoveredFromBackup() 为真、
    // lastBackupRecoveryReason() 带主文件损坏原因，open 仍算成功。
    // catalogPath() 仍返回 catalog.json 路径，不表示这次打开写了 JSON。
    bool open(const QString &projectDir, QString *error = nullptr);
    // Background preparation closes SQLite on its creating thread. Adoption
    // transfers parsed tables/indexes to this stable QObject without re-reading.
    static std::shared_ptr<DataCatalog> prepareOpen(const QString &projectDir, bool readOnly);
    bool adoptPrepared(const std::shared_ptr<DataCatalog> &prepared, QString *error = nullptr);
    bool isOpen() const { return m_isOpen; }
    // 拒绝写入态：open() 失败、从未成功 open 过、或被锁降级只读 → true。
    // mutator 一律 return false + error，不落盘不改内存——绝不让空 catalog
    // 覆盖坏文件（audit row 36，§9 回滚语义）。
    bool refusesWrites() const { return !m_isOpen || m_lockedReadOnly; }
    QString openError() const { return m_openError; } // open() 失败原因（无则空）
    QString catalogPath() const { return m_dir + QStringLiteral("/artifacts/metadata/catalog.json"); }
    // 工程根目录（open 的入参原样；未 open = 空）。受管相对路径解析与
    // 面板路径问答用（resolvedVersionPath 的第一参）。
    QString projectDir() const { return m_dir; }
    // 即使 store 还没建也返回该路径。不表示文件一定存在。
    QString sqliteCatalogPath() const
    {
      return m_dir + QStringLiteral("/artifacts/metadata/catalog.sqlite");
    }
    // 当前内存表写成 JSON。revision 用现值（不 +1）。不写 sqlite、不轮转
    // bak、不涨 revision、不发 changed()。
    bool exportCatalogJson(const QString &path, QString *error = nullptr) const;

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
    // 审计 02 M-8：内存变更序号——每次成功的 mutator 与 next*Id 分配都 +1
    //（不落盘、不随批次延迟）。ImportSession 以它做提交基线：produce 期间
    // owner 侧任何写入/分配都会让提交返回 Conflict。
    quint64 mutationSeq() const { return m_mutationSeq; }

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
        void abort(); // 回滚整个外层批次；嵌套作用域结算时统一生效
      private:
        DataCatalog *m_catalog = nullptr;
        bool m_done = false;
    };

    // 变更：每次落盘（原子写：temp + rename），revision 单调递增。
    bool addEntity(const CatalogEntity &e, QString *error = nullptr);
    bool addAsset(const CatalogAsset &a, QString *error = nullptr);
    bool addVersion(const CatalogVersion &v, QString *error = nullptr);
    bool addLink(const EntityAssetLink &l, QString *error = nullptr);

    // 版本 extra 就地更新（方向 79 锚深后补编辑通道）。契约（定案判据
    // 「审计面与撤销语义」记录于 ledger 与 docs/progress）：导入元数据
    // 修正走就地 update——不新建版本、不动 versionNumber/path/sha256/
    // parentVersionIds（锚深是导入元数据而非派生结果；落 DERIVED 新版本
    // 会 fork 血统且让 currentVersion 读取面复杂化）。审计：改前值压入
    // extra["<key>#history"]（{v,at} 数组，FIFO 至多 8 条），
    // extra["<key>#source"]="manual"（导入侧写 "filename"）。
    // value 为无效 QVariant → 删除键（清锚 = 回到「未锚定」态）。
    // 撤销 = 用 history 旧值再调本函数（对称操作）。未知 versionId /
    // 空 key / 带 '#' 保留字 → false + error（#history/#source 是审计面，
    // 不开放为用户键）。staging 副本上正常可用（journal 记 op；重放重算
    // history——值序确定，时间戳取重放时刻，审计语义不变）。
    bool updateVersionExtra(const QString &versionId, const QString &key,
                            const QVariant &value, QString *error = nullptr);

    // 把 links() 序中第 index 条未决链接挂到 entityId：置已决、清备注。
    // 方向 44 挂接契约（收口）：主文件只由显式操作变更——挂接不夺主。
    // 目标 (entityType, entityId, role) 已有已决主关联 → 本链接为成员
    // （isPrimary=false，现任主不动）；无主 → 补主（首份语义）。显式夺主
    // 唯一入口是 setLinkPrimary（「设为主文件」）。§3 不变量不变：同角色
    // 至多一条主关联。index 越界 / 链接已决 / entityId 为空 → false。
    bool attachLink(int index, const QString &entityId, QString *error = nullptr);

    // attachLink 的撤销面（资产表「未决」行的会话内回退，§4）：links() 序第
    // index 条已决链接改回未决——entityId 清空、unresolved=true、isPrimary 降级、
    // 备注清空。资产与被共享的井实体都保留（未决不是删除）。index 越界或链接
    // 本来就是未决 → false。
    bool setLinkUnresolved(int index, QString *error = nullptr);

    // 「将此版本设为主版本」（§4）：links() 序第 index 条已决链接提升为同角色
    // 主关联——本链接 isPrimary=true，同一 (entityType, entityId, role) 下的
    // 其他主关联降级（§3 不变量）。这是变更主文件的唯一显式入口（方向 44
    // 挂接契约的另一半）。只动链接标志，不复制版本字节。index 越界 /
    // 链接未决或实体 id 为空 → false。
    bool setLinkPrimary(int index, QString *error = nullptr);

    // 物理删除资产（方向 30 回收站「物理删除」的 catalog 面）：一次事务删除
    // 资产行 + 其全部版本行 + 其全部链接行。血缘保护——待删版本被其他资产的
    // 版本列为 parentVersionIds 时拒绝（拆血统必须显式先删引用方资产）；
    // 实体行保留（实体可能仍挂别的资产；孤儿实体的发现归体检面）。磁盘上的
    // 受管字节不在此职责内——调用方（workflow/assetops purgeAssets）在提交
    // 成功后清理。staging 副本拒绝（journal 无删除 op，重放不了）。
    bool removeAsset(const QString &assetId, QString *error = nullptr);
    bool removeAssets(const QStringList &assetIds, QString *error = nullptr);
    // Governance mutator: exact stale DERIVED versions only, one atomic save.
    // Retained descendants block removal; asset/link identities remain intact.
    bool removeStaleVersions(const QStringList &versionIds, QString *error = nullptr);

    // 更新版本 / 资产当前版本 extra 字典（合并键值，落盘原子写）
    bool updateVersionExtra(const QString &versionId, const QVariantMap &extra, QString *error = nullptr);
    bool updateAssetExtra(const QString &assetId, const QVariantMap &extra, QString *error = nullptr);

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
    static QString sha256FileHex(const QString &path, QString *error = nullptr,
                                const std::function<bool()> &cancelled = {});

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
    bool writeSurveyGeoJson(const QString &path, QString *error = nullptr) const;

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

    // ---- 审计 02 M-8：produce-then-commit（导入 worker 不碰活 catalog）----
    // 线程纪律：活 catalog 只被它所属线程（thread()）读写。跨线程写一律拒绝
    // （return false + error，计入 threadViolationCount 并 qCritical；环境变量
    // PALEO_CATALOG_STRICT_THREADS=1 时直接 qFatal）；跨线程读计数 + 告警。
    static int threadViolationCount();
    static void resetThreadViolationCount();

    // owner 线程调用：拷一份「staging 副本」交给一个 worker 独占使用
    // （四表/索引/序号 COW 拷贝）。副本的 save() 不落盘、只在内存生效；每个
    // 成功的 mutator 记进 journal()。overlayDir 是 worker 写受管文件的私有
    // 暂存根（与工程目录同构：<overlay>/artifacts/raw/...）——副本上新增的
    // 受管版本按它解析文件（dedup 复核 / 同批派生件存在性），见 versionFilePath。
    std::unique_ptr<DataCatalog> createStagingCopy(const QString &overlayDir) const;
    bool isStaging() const { return m_staging; }
    QVector<CatalogOp> journal() const { return m_journal; }
    bool isStagedVersion(const QString &versionId) const
    {
      return m_overlayVersionIds.contains(versionId);
    }
    // 版本文件绝对路径：staging 副本上新增的受管版本解析到暂存根，其余同
    // resolvedVersionPath(projectDir, v)。
    QString versionFilePath(const CatalogVersion &v) const;
    // owner 线程：把 journal 原样重放进本 catalog——全部成功才落一次盘；任一
    // op 失败或落盘失败 → 内存逐字段还原、盘上不动（事务语义），返回 false。
    // 空 journal → true 且不落盘（不空涨 revision）。
    bool applyJournal(const QVector<CatalogOp> &ops, QString *error = nullptr);
    // 测试注入：重放恰好成功 k 个 op 后、endBatch/落盘之前还原并失败。
    // k<=0 记 0 并关闭。applyJournal 不自动清零（非零粘滞会让失败后的重试也中止）。
    void debugAbortJournalAfter(int k);

  signals:
    void changed();      // 任一变更落盘后发射（UI 刷新资产表用）
    // open() 经 .bak 回退恢复成功后发射一次（主文件损坏原因随行）——UI/
    // 状态面据此向用户告警「catalog 已从备份恢复，主文件损坏」。
    void backupRecovered(const QString &reason);

  private:
    bool ensureOpen(QString *error) const;
    bool checkWriteThread(const char *what, QString *error) const;
    void noteRead(const char *what) const;
    void recordOp(CatalogOp op);
    bool save(QString *error = nullptr);
    bool commitStore(QString *error);
    void beginBatch();
    bool endBatch(QString *error = nullptr);
    bool m_batchAborted = false;
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
    bool m_preparedOpen = false;
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
    // 审计 02 M-8 staging 副本态（见 createStagingCopy）
    bool m_staging = false;
    quint64 m_mutationSeq = 0;
    QString m_overlayDir;
    QSet<QString> m_overlayVersionIds;
    QVector<CatalogOp> m_journal;
    // 活库连接。staging 副本不拷贝（默认空）——副本不 open、不落盘。
    std::unique_ptr<CatalogStore> m_store;
    bool m_forceFullSave = false; // 损坏主库已装入内存，下次 save 整表重写
    int m_debugAbortJournalAfter = 0; // 见 debugAbortJournalAfter；0 = 关闭
    QSet<QString> m_dirtyEntities;
    QSet<QString> m_dirtyAssets;
    QSet<QString> m_dirtyVersions;
    QSet<int> m_dirtyLinkOrds;
    // 增量删除面（removeAsset）：upsert 之后执行——先重写后删，净效果=已删。
    QSet<QString> m_removedAssets;
    QSet<QString> m_removedVersions;
    bool m_linksFullRewrite = false; // 删链接行 → 后续 ord 整体位移，只能整表重写
    // #169：最外层批次开始时的内存快照。批次结算落盘失败 → 回滚到此，避免
    // 「内存有、盘上无、脏集已清永不再写」的分叉。四表 COW，拍照近乎零成本。
    struct BatchSnapshot
    {
      QVector<CatalogEntity> entities;
      QVector<CatalogAsset> assets;
      QVector<CatalogVersion> versions;
      QVector<EntityAssetLink> links;
      int assetSeq = 0, versionSeq = 0;
      CatalogIndex idx;
      QSet<QString> dirtyEntities, dirtyAssets, dirtyVersions;
      QSet<int> dirtyLinkOrds;
      QSet<QString> removedAssets, removedVersions;
      bool linksFullRewrite = false;
    };
    std::unique_ptr<BatchSnapshot> m_batchSnapshot;
};
