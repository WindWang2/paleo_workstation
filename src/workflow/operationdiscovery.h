// 层：功能
// workflow/operationdiscovery.h — 当前 Agent 作用域的持久 operation 只读发现。
// paleo.operation_list 的后端：一次受权读取形成不可变摘要快照，分页从快照
// 顺序取出；后续当前状态仍须 operation_get 对账。本服务不写 journal、不
// 执行/恢复/对账业务、不建第二份可写账本——缓存里只有摘要与关系事实。
#pragma once
#include "capabilitygateway.h"
#include <QJsonObject>
#include <QString>
#include <memory>
#include <functional>

namespace paleo::capability {

inline constexpr char OperationDiscoveryContract[] = "paleo.operation-discovery.v1";

struct DiscoveryPolicy {
  int maxSnapshots = 4;                    // 同时保留的快照数（只回收过期项）
  int maxCursors = 128;                    // 未过期游标数上限
  qint64 ttlMs = 10 * 60 * 1000;           // 快照与游标 TTL
  qint64 maxCacheBytes = 8 * 1024 * 1024;  // 快照摘要总字节上限
};

// 宿主（owner 线程）冻结后交给 worker 的只读扫描输入。documentId/callerScope
// 一律来自可信 Host.callerScope(Agent,currentChatSessionId)，不接受实参、
// 历史 JSON 或游标自授；writeClaim 仅用于受管路径钉住（符号链接/联接别名
// 防御），本服务永不调用写路径。
struct DiscoveryScan {
  QString projectDir;   // canonical
  QString operationDir; // trusted host value
  QString documentId, callerScope;
  std::shared_ptr<const ProjectWriteClaim> writeClaim;
};

// 线程边界：query() 在 TaskService worker 上执行（磁盘/解析不占 owner），
// invalidate()/setClock() 在 owner 上执行；内部互斥保证一致。服务不持有
// 活 Host/Catalog/GUI 对象，可被 shared_ptr 保活至 worker 安全结束。
class OperationDiscovery {
public:
  explicit OperationDiscovery(DiscoveryPolicy policy = {});
  ~OperationDiscovery();
  // 会话/工程/epoch 变化：丢弃全部快照与游标（旧上下文的分页不再可用）。
  void invalidate();
  // 一次查询：无 cursor 时先扫描建快照，再从快照取页；有 cursor 时从绑定
  // 快照续页。实参校验先于任何磁盘读取。返回值即模型可见 envelope。
  QJsonObject query(const DiscoveryScan &scan, const QJsonObject &arguments,
                    const std::function<bool()> &cancelled = {});
  // 诊断（测试/观测口径）：全量扫描次数、缓存保留字节、存活快照/游标数。
  int scanCount() const;
  qint64 retainedBytes() const;
  int liveSnapshots() const;
  int liveCursors() const;
  // 注入时钟（测试 TTL/过期）；缺省墙钟。
  void setClock(std::function<qint64()> clock);

private:
  // 以下两个 helper 由 query() 在已持有 Impl 互斥的路径上调用。
  QJsonObject pageFromCursor(const DiscoveryScan &scan, const QString &digest, const QString &cursor,
                             bool relations, const QString &anchor, int pageSize, int maxBytes);
  QJsonObject pageFromSnapshotLocked(const QString &snapshotKey, int position, bool relations,
                                     const QString &anchor, int pageSize, int maxBytes,
                                     const QString &consumedCursor);
  struct Impl;
  std::unique_ptr<Impl> m;
};

} // namespace paleo::capability
