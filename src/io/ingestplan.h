// 层：数据
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

#include "../catalog/datacatalog.h"
#include "../domain/importrows.h" // FolderRowResult（执行器返回行）
#include "dataimportservice.h"

// io/ — IngestPlan 三段式（docs/DATA_FABRIC_ADOPTION.md C 包；
// 参考 paleo_project/main/libs/data_suite ingest_plan.hpp 的语义模型，只采纳
// 模型不移植代码）：把「导入工区文件夹」的 扫描+确认+执行 显式分层——
//
//   1) buildIngestPlan(root, catalog) 纯函数：扫描 → 分类 → shp 族归组 →
//      身份匹配 → sha256 去重 → suggestedPrimary。不落盘不改 catalog
//      （活 catalog 只在它自己的线程查询；后台 produce 对 ImportSession 的
//      staging 副本构建——审计 02 M-8）。
//   2) 确认框按行展示决策（重复→跳过 / 重复→新版本；未决保持原显示）。
//   3) executeIngestPlan 幂等执行：decision==skip 或 (path,sha) 已注册 →
//      跳过；as_new_version 有意绕过幂等检查——同字节重登记由内部 dedup
//      消化（AlreadyStored + 补挂）。produce 写 session 的 staging 副本 +
//      协作取消；owner 线程 commitImport 一次事务入库。
//
// 与参考实现的已知差异：去重键是「sha 命中任一已注册版本」（我方 catalog
// §3 dedup 口径），不是对方的 (source_uri, sha) 对——同字节不同来源再导入
// 仍命中 dedup 走 AlreadyStored；shp 族不按对方习惯改判 geojson——保留
// 分类器类型，导入时复制全组、sha 取主件（.shp）本身（versionBySha256 的
// 复核语义按单文件算，聚合 sha 会让已注册版本复核失配）。
struct PlannedItem
{
  QString path;            // 源路径（所选目录内；族项 = 主件 .shp 路径）
  QString canonicalPath;   // 枚举时刻 canonicalFilePath——T33 TOCTOU 复核基准
  QString type;            // 分类器类型；确认表「改类型」override 后 = 生效类型
  QString format;          // 小写扩展名
  qint64 size = 0;
  QString sha256;          // ≤200MB 才算；SEG-Y 等大文件留空（plan 期不去重）
  QStringList members;     // shp 族全部成员路径（含主件，按路径排序）；非族为空
  QString entityType, entityId, entityName; // 身份匹配结果（id 空=未决/新建）
  bool entityAmbiguous = false;             // 双候选 → unresolved，不猜
  QString role;            // 推断角色（well_log/tops/time_depth/...）
  bool suggestedPrimary = true;             // 同 (entity,role) 首成员建议主关联
  QString duplicateOfVersionId;             // plan 期 sha 去重命中版本 id
  QString decision = QStringLiteral("accept"); // accept | skip | as_new_version
  QString note;
};

struct IngestPlan
{
  QString root;
  QVector<PlannedItem> items;    // 两阶段序：well_head 先行、其余按路径；族已归并
  QVector<PlannedItem> skipped;  // 枚举期跳过（逃逸链接/非普通文件），按路径序
  QStringList issues;            // 不可读文件、坏根目录、哈希失败等
  bool cancelled = false;        // scanProgress 返回 false 中途放弃（issues 另记「已取消」）

  // 未决口径：井类行（well_log / well_stratification / time_depth）一个已决
  // 实体都没匹配到（entityId 空 = 零匹配或只剩歧义候选；entityAmbiguous 是
  // 它的细化标记）。well_head 是建井来源不算未决；一个歧义名字也不猜。
  int unresolvedCount() const
  {
    int n = 0;
    for (const PlannedItem &it : items)
      if (it.entityId.isEmpty() &&
          (it.type == QLatin1String("well_log") ||
           it.type == QLatin1String("well_stratification") ||
           it.type == QLatin1String("time_depth")))
        ++n;
    return n;
  }
  int duplicateCount() const
  {
    int n = 0;
    for (const PlannedItem &it : items)
      if (!it.duplicateOfVersionId.isEmpty())
        ++n;
    return n;
  }
};

// 每处理完一项回调一次 (done, total, 该项路径)；返回 false = 协作取消
// （与 DataImportService::importFolder 的 progress 同一口径）。
using IngestProgress = std::function<bool(int done, int total, const QString &path)>;

// plan 构建期（扫描/哈希）进度回调：每见一个源文件回调一次 (已见文件数,
// 该文件路径)；返回 false = 协作取消（plan 停在半途，已收项保留，issues
// 记「已取消」）。total 语义在 plan 期不可知（边扫边发现），调用方以
// total=0 与执行期 progress 区分。
using IngestScanProgress = std::function<bool(int filesSeen, const QString &path)>;

// ---- plan 构建器的 catalog 只读面（T2：plan 期搬出 GUI 线程）----
//
// buildIngestPlan 只消费这 5 个查询。两个实现：
//   · LiveCatalogSource——直接包 DataCatalog：只能在 catalog 所在线程用
//     （GUI 直调零拷贝，等价旧路径）；
//   · CatalogReadSnapshot——在 catalog 线程上 fromCatalog() 拷出（QVector
//     隐式共享 → O(1)），之后任意线程只读安全：worker 线程构建 plan 从此
//     不经 BlockingQueuedConnection marshal 回 GUI——扫描/分类/哈希/身份
//     匹配全在 worker，GUI 只收进度。快照是构建瞬间的一致性视图；执行
//     期写入落 ImportSession 的 staging 副本，owner 线程提交时按基线
//     revision 校验（变了 → Conflict，调用方重做）。
class IngestCatalogSource
{
  public:
    virtual ~IngestCatalogSource() = default;
    virtual bool isOpen() const = 0;
    virtual QString projectDir() const = 0; // 工程子树守卫；未 open → 空
    virtual QStringList wellsMatchingName(const QString &name) const = 0;
    virtual CatalogEntity entityById(const QString &id) const = 0;
    virtual CatalogVersion versionBySha256(const QString &sha256) const = 0;
    virtual QVector<EntityAssetLink> linksForEntity(const QString &entityId) const = 0;
};

// DataCatalog 的活对象只读包装（catalog 线程专用；零拷贝直通）。
class LiveCatalogSource : public IngestCatalogSource
{
  public:
    explicit LiveCatalogSource(const DataCatalog *catalog) : m_cat(catalog) {}
    bool isOpen() const override;
    QString projectDir() const override;
    QStringList wellsMatchingName(const QString &name) const override;
    CatalogEntity entityById(const QString &id) const override;
    CatalogVersion versionBySha256(const QString &sha256) const override;
    QVector<EntityAssetLink> linksForEntity(const QString &entityId) const override;

  private:
    const DataCatalog *m_cat = nullptr;
};

// COW 快照：catalog 线程上 fromCatalog() 拷出后任意线程只读。
// versionBySha256 会重哈希版本文件（文件 IO 在调用线程执行——worker 上
// 跑正合适，这正是要搬出 GUI 的重活之一）。
// WP2：fromCatalog 一次性建四张命中索引（sha(lower)→行集、实体 id→行、
// 规范化井名→id 列表、实体 id→链接行集）——buildIngestPlan 逐项调
// versionBySha256/entityById/wellsMatchingName/linksForEntity，旧实现每次
// 线性扫快照表，N 文件目录的 plan 构建即 O(N²)。
class CatalogReadSnapshot : public IngestCatalogSource
{
  public:
    static CatalogReadSnapshot fromCatalog(const DataCatalog &catalog);
    bool isOpen() const override { return m_open; }
    QString projectDir() const override { return m_projectDir; }
    QStringList wellsMatchingName(const QString &name) const override;
    CatalogEntity entityById(const QString &id) const override;
    CatalogVersion versionBySha256(const QString &sha256) const override;
    QVector<EntityAssetLink> linksForEntity(const QString &entityId) const override;

  private:
    bool m_open = false;
    QString m_projectDir;
    QString m_dir; // 版本路径解析根（resolvedVersionPath 用）
    QVector<CatalogEntity> m_entities;
    QVector<CatalogVersion> m_versions;
    QVector<EntityAssetLink> m_links;
    QHash<QString, QVector<int>> m_rowsBySha;       // sha(lower) → 版本行集（升序）
    QHash<QString, int> m_rowByEntityId;            // 实体 id → 行
    QHash<QString, QStringList> m_wellIdsByNormName; // 规范化井名 → 井 id（表序）
    QHash<QString, QVector<int>> m_rowsByEntityId;  // 实体 id → 链接行集（升序）
};

// 纯函数：root 可为目录（递归枚举）或单文件。catalog 只读面由调用方给
// （活对象仅 catalog 线程；快照任意线程——见 IngestCatalogSource）。
// scanProgress 可选：plan 构建期逐文件进度 + 协作取消。
IngestPlan buildIngestPlan(const QString &root, const IngestCatalogSource &catalog,
                           const IngestScanProgress &scanProgress = {});

// 旧签名保留（GUI 线程零拷贝直通）：等价于 LiveCatalogSource 包装。
IngestPlan buildIngestPlan(const QString &root, const DataCatalog &catalog);

// 按生效类型把 items 重排成两阶段序（well_head 先行）——buildIngestPlan 已
// 排好；确认表 override 改了类型之后要再调一次（改回 well_head 的行回阶段 1）。
void orderIngestPlanItems(QVector<PlannedItem> &items);

// 执行器（produce 面，任意线程）：幂等（decision==skip 或 path+sha 已注册 →
// 跳过）、progress 协作取消（已处理行保留，error=「已取消（已入库的行保
// 留）」）。只写 session 的 staging 副本——返回行结果与 importFolder 同一
// 口径（plan.skipped 行缀在最后），入库由 owner 线程 commitImport 完成。
QVector<FolderRowResult>
executeIngestPlan(const IngestPlan &plan, ImportSession &session,
                  const IngestProgress &progress = {}, QString *error = nullptr);
// owner 线程便捷面：beginImport → 上面的 produce → commitImport（整 plan
// 一次事务落盘；提交失败 → 行改 Failed + error）。别的线程调用如实失败。
QVector<FolderRowResult>
executeIngestPlan(const IngestPlan &plan, DataImportService &svc,
                  const IngestProgress &progress = {}, QString *error = nullptr);
