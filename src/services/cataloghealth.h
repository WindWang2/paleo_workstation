// 层：数据
#pragma once
#include <QString>
#include <QVector>
#include <functional>

class DataCatalog;
struct CatalogVersion;

// services/cataloghealth — 工区资产体检（方向 30：健康度仪表盘数据面）。
// 纯查询：复用 catalog 现有检测原语（resolvedVersionPath 的受管路径安全
// 校验、unresolvedLinks/invalidRoleLinks、版本表/链接表邻接查询），不重造
// 检测、不写 catalog、不碰索引缓存。
//
// 两段式（线程纪律：活 catalog 只在 GUI 线程读——重活必须离线）：
//   buildCatalogHealth —— 快速面（stat + 内存查询），GUI 线程毫秒级：
//     MissingFile     版本文件缺失（受管：工程内路径不存在/不安全；外链：
//                     源路径不存在）——扫全部版本行，不只 currentVersion
//                     （历史版本缺文件同样阻断回滚/对比）。
//     PendingLink      未决链接（entityId 留空的行，note 记候选/未匹配名）。
//     OrphanEntity     无任何链接的实体。
//     NoVersionAsset   无版本行的资产（catalog 契约允许，但消费面全瘫）。
//     InvalidRoleLink  角色词表违例链接（roleDiagnosis 写 note 的机制既有）。
//   verifyExternalShas —— 外链 SHA-256 复验（流式重算大文件，重活）：输入
//     是版本值拷贝 + 静态哈希函数，不触碰 DataCatalog 成员——可在 worker
//     线程跑，无跨线程读告警。逐版本回调进度，返回 false 协作取消。
namespace paleo::health
{

enum class IssueKind
{
  MissingFile,
  ShaMismatch,
  PendingLink,
  OrphanEntity,
  NoVersionAsset,
  InvalidRoleLink,
  StaleVersion // 方向 36：与血缘图共享 extra[stale]/staleReason，不再另算状态。
};

struct HealthIssue
{
  IssueKind kind = IssueKind::MissingFile;
  QString assetId;    // 资产类问题非空；双击跳资产
  QString versionId;  // 版本级问题（MissingFile/ShaMismatch）非空
  QString entityId;   // OrphanEntity 非空；跳实体
  QString subject;    // 显示主体（资产 displayName / 实体名 / 文件名）
  QString detail;     // 机器侧原文：路径、SHA、note、role——不猜不修饰
  qint64 sizeBytes = -1;
};

struct HealthReport
{
  QVector<HealthIssue> issues;
  // SHA 复验段被取消时 false——ShaMismatch 计数只是已扫部分的召回，
  // UI 需如实标「未扫完」。
  bool shaVerifyComplete = true;

  int count(IssueKind kind) const
  {
    int n = 0;
    for (const HealthIssue &i : issues)
      if (i.kind == kind)
        ++n;
    return n;
  }
  bool isEmpty() const { return issues.isEmpty(); }
};

// cat 为空/未开 → 空报告 + *error（UI 据此显示「工程未打开」而不是假绿灯）。
HealthReport buildCatalogHealth(DataCatalog *cat, const QString &projectDir,
                                QString *error = nullptr);

// 外链带 SHA 版本的离线复验：对每个版本流式重算源文件 SHA-256 与留底比对。
// versions 是调用方在 GUI 线程拷出的值列表（只含 !managed && !sha256.isEmpty()
// 的行——本函数再过滤一次防混入）。subject 用版本 fileName（worker 拿不到
// 资产表，不回查 catalog）。progress(已验数, 总数, 文件名) 返回 false = 取消。
QVector<HealthIssue> verifyExternalShas(const QVector<CatalogVersion> &versions,
                                        const std::function<bool(int, int,
                                                                 const QString &)> &progress = {});

} // namespace paleo::health
