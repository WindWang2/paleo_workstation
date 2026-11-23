// 层：功能
#pragma once
#include "../ai/chat/chatsessionindex.h"
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QVector>
#include <memory>

class PaleoTask;
class PaleoTaskService;

// H2（agent-session-archive，2026-11-22）：最近会话元信息查询协调器。
//
// 热路径（每次刷新）：目录扫描（只 stat，不读正文）→ 内存命中直用 → 未命
// 中读该会话自己的 sidecar（≤2 KiB）→ 仍不新鲜的条目进待修复队列。limit
// 固定时不再载入所有正文对象排序；无候选即发布 complete 结果。
//
// 冷修复：候选经既有 PaleoTaskService 的 quiet 低优先应用任务在池线程逐个
// 重建 sidecar（有界读 + 权威校验；每文件之间协作取消、支持外部换代的
// 单飞约束）。修复完成回到主线程重扫热路径并发布 complete 结果。无任务
// 服务（未装配/测试直连控制器）时不做修复——只发布已有可信缓存的部分
// 结果（complete=false），绝不退回 GUI 线程全库正文解析。
//
// 一致性（W6）：内存缓存与 sidecar 都只是派生缓存——权威 save/remove 之后
// 的新鲜度戳不匹配会让条目自动进入待修复；进程在权威写入与缓存更新之间
// 终止、外部删除/替换、快速连续 checkpoint，都收敛到磁盘事实。缓存错误不
// 触发正文清洗，也不反向影响 G2 保存欠账语义。
class AiChatRecentSummaries : public QObject {
  Q_OBJECT
public:
  explicit AiChatRecentSummaries(QObject *parent = nullptr);
  ~AiChatRecentSummaries() override;
  void setTaskService(PaleoTaskService *service);
  bool repairInFlight() const { return m_repairTask != nullptr; }
  // 触发一次查询；结果（含热路径同步完成的 complete 结果）经 ready 发布。
  void refresh(int limit);

signals:
  // complete=false：仍有待修复条目或扫描被截断——有限扫描的部分结果，
  // 修复完成后会再次发布；不得把它当成完整最近集合。
  void ready(const QVector<paleo::chat::ChatSessionSummary> &summaries,
             bool complete);

private:
  struct CacheEntry {
    paleo::chat::ChatSessionSummary summary;
    qint64 bytes = 0;
    qint64 mtimeMs = 0;
  };
  // worker（池线程）与协调器共享的产物护栏：shared_ptr 双方持有，先亡
  // 不 UAF；互斥保护回填窗口。
  struct RepairRun {
    QMutex mutex;
    QVector<paleo::chat::ChatArchiveRepairItem> items;
    int vanished = 0;
    int irregular = 0;
  };
  void startRepair(const QVector<paleo::chat::ChatArchiveFileStamp> &candidates);
  QHash<QString, CacheEntry> m_cache;
  QPointer<PaleoTaskService> m_service;
  QPointer<PaleoTask> m_repairTask;
  std::shared_ptr<RepairRun> m_run;
  int m_limit = 20;
  bool m_repairSuppressed = false; // sidecar 写失败后停止自动重试（防循环）
  QVector<paleo::chat::ChatArchiveFileStamp> m_failedCandidates; // 触发抑制的
  // 候选指纹：候选集合/指纹变化（新保存、外部改动）即恢复修复资格。
};
