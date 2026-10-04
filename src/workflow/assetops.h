// 层：功能
#pragma once
#include <QString>
#include <QStringList>
#include <QVector>

class DataCatalog;
struct CatalogVersion;

// workflow/assetops — 资产级数据面操作（方向 30：版本回滚 / 回收站物理清理 /
// 未决链接批量归位）。一切改动经 catalog API（addVersion/removeAsset/
// attachLink）走版本与事务，禁旁路写文件、禁直改索引缓存；文件操作只发生在
// catalog 行已提交（回滚拷贝目标）或已删除（清理受管字节）之后。
namespace paleo::assetops
{

// ---- 版本回滚（回滚 = 新版本指向旧内容，不删历史）----
struct RollbackOutcome
{
  QString versionId;    // 新版本 id（空 = 失败）
  int versionNumber = 0;
  QString sha256;       // 与目标版本一致的留底摘要
};

// 把 assetId 回滚到 targetVersionId 的内容：新增一条 versionNumber = 当前
// 最大 +1 的版本行——受管目标 = 把目标版本文件字节拷进新受管路径（拷后重算
// SHA-256，与目标留底不一致即失败清理，防「回滚出被篡改的内容」）；外链目标 =
// 新版本记录指向同一源路径与留底 SHA（零拷贝）。parentVersionIds = {目标}，
// extra["rollbackOf"] 留血统。单 BatchSave 单事务落盘。
RollbackOutcome rollbackToVersion(DataCatalog *cat, const QString &projectDir,
                                  const QString &assetId,
                                  const QString &targetVersionId,
                                  QString *error = nullptr);

// ---- 两版本对比（元数据级 + 文本行级 best-effort）----
struct VersionFieldDiff
{
  QString field;
  QString a;
  QString b;
};

struct VersionCompare
{
  bool shaKnown = false;   // 任一侧缺留底 SHA → false（不假装可比）
  bool sameSha = false;
  qint64 sizeA = -1;       // stat 不到 = -1
  qint64 sizeB = -1;
  QVector<VersionFieldDiff> fieldDiffs; // stage/managed/sourceUri/fileName/parents
  bool textCompared = false;            // 双侧可读文本且 ≤ kMaxTextDiffBytes
  int linesA = 0;                       // 差异块行数（首差异块，非全量 LCS）
  int linesB = 0;
  QStringList differingLines;           // 首差异块样本（带 "-"/"+" 前缀，封顶条）
};

static constexpr qint64 kMaxTextDiffBytes = 2 * 1024 * 1024;

VersionCompare compareVersions(const QString &projectDir, const CatalogVersion &a,
                               const CatalogVersion &b);

// ---- 回收站物理清理（磁盘 + catalog 双清）----
struct PurgeOutcome
{
  QStringList purgedAssetIds; // catalog 行与文件都清掉的
  QStringList failedAssets;   // "<资产名>: 原因"（血缘保护/不存在/落盘失败）
  QStringList leftoverFiles;  // catalog 已删但文件删不掉的残留绝对路径
  qint64 bytesFreed = 0;      // 实际删掉的受管文件字节（外链源不计——不碰工程外文件）
};

// 物理删除：一次 BatchSave 里 removeAsset 全部入选资产（单事务——flush 失败
// 整批回滚、零文件动作）；提交成功后删受管版本文件与空目录（best-effort，
// 残留如实报）。外链版本源文件永远不删（工程外的字节不是我们的）。目标版本
// 被其他资产的 DERIVED 版本引用（parentVersionIds）时 removeAsset 拒绝——
// 血统保护在 catalog 层，这里只透传原因。
PurgeOutcome purgeAssets(DataCatalog *cat, const QString &projectDir,
                         const QStringList &assetIds);

// ---- 未决链接批量归位（与导入同一判据，不猜）----
struct PendingProposal
{
  int linkIndex = -1; // links() 序（提议与执行之间 catalog 不应被改写）
  QString assetId;
  QString assetName;
  QString wellId;
  QString wellName;
  QString sourceName; // 判定用的名字来源（文件名主名或 note 未匹配名）
};

// 扫全部未决井类链接，列出「现在恰好命中一口井」的可归位项：
//   候选名 = 当前版本文件名主名 → note「未匹配井名: X」（零匹配备注）；
//   wellsMatchingName 恰好 1 个才提议；2+ 候选或 0 个不提议（与导入判据
//   一致，不猜）；目标 (well, role) 已有已决主关联的不提议（没有可补的位）。
QVector<PendingProposal> proposablePendingLinks(DataCatalog *cat);

// 应用选中的归位项（linkIndexes = links() 序号集合）：attachLink 逐条 +
// 单 BatchSave 单事务。返回实际挂上的条数；落盘失败 → 0 + *error（整批回滚）。
int applyPendingResolutions(DataCatalog *cat, const QVector<int> &linkIndexes,
                            QString *error = nullptr);

} // namespace paleo::assetops
