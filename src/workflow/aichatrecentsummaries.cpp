// 层：功能
#include "aichatrecentsummaries.h"
#include "../ai/chat/chatsession.h"
#include "../services/paleotaskservice.h"

#include <algorithm>

namespace {
// 扫描上限：目录 stat 数量的护栏（正常用户会话量远低于此；超出即如实
// 报 partial，不假装是完整最近集合）。
constexpr int kMaxScanEntries = 4096;
} // namespace

AiChatRecentSummaries::AiChatRecentSummaries(QObject *parent) : QObject(parent) {}

AiChatRecentSummaries::~AiChatRecentSummaries() = default;

void AiChatRecentSummaries::setTaskService(PaleoTaskService *service) {
  m_service = service;
}

void AiChatRecentSummaries::refresh(int limit) {
  m_limit = limit;
  const QString root = ChatSessionStore::directory();
  const auto scan = paleo::chat::scanChatArchiveFiles(root, kMaxScanEntries);
  QVector<paleo::chat::ChatSessionSummary> summaries;
  QVector<paleo::chat::ChatArchiveFileStamp> candidates;
  QHash<QString, CacheEntry> cache; // 重建式缓存：只保留本轮仍命中的条目
  summaries.reserve(scan.files.size());
  for (const paleo::chat::ChatArchiveFileStamp &stamp : scan.files) {
    const auto cached = m_cache.constFind(stamp.id);
    if (cached != m_cache.constEnd() && cached->bytes == stamp.bytes &&
        cached->mtimeMs == stamp.mtimeMs) {
      summaries.append(cached->summary);
      cache.insert(stamp.id, *cached);
      continue;
    }
    paleo::chat::ChatSessionIndex::Entry entry;
    if (paleo::chat::ChatSessionIndex::read(stamp.id, &entry) &&
        entry.sourceBytes == stamp.bytes &&
        entry.sourceMtimeMs == stamp.mtimeMs) {
      CacheEntry updated{entry.toSummary(), stamp.bytes, stamp.mtimeMs};
      summaries.append(updated.summary);
      cache.insert(stamp.id, updated);
      continue;
    }
    candidates.append(stamp); // 缓存缺失/损坏/过期：待修复（不读正文）
  }
  m_cache = cache;
  // 孤儿缓存清理（W6）：外部删除的正文不在扫描集里——只清缓存目录里的
  // sidecar，永不触碰任何正文文件。扫描被截断（>上限的窗口子集）时跳过：
  // liveIds 不完整，窗口外的 sidecar 不能当孤儿删（缓存 churn 自愈面收窄
  // 到真实孤儿）。
  if (!scan.truncated) {
    QStringList liveIds;
    liveIds.reserve(scan.files.size());
    for (const paleo::chat::ChatArchiveFileStamp &stamp : scan.files)
      liveIds.append(stamp.id);
    paleo::chat::ChatSessionIndex::pruneOrphans(liveIds);
  }
  // 排序契约：updatedAt 优先、无效回退 createdAt（与旧 loadRecent 同义）；
  // 无任何合法时间的条目排最后；同值按 id 升序——同秒批量写入也确定。
  const auto stampOf = [](const paleo::chat::ChatSessionSummary &s) {
    return s.updatedAt.isValid() ? s.updatedAt : s.createdAt;
  };
  std::stable_sort(summaries.begin(), summaries.end(),
                   [&](const paleo::chat::ChatSessionSummary &a,
                       const paleo::chat::ChatSessionSummary &b) {
                     const auto keyA = stampOf(a), keyB = stampOf(b);
                     const bool validA = keyA.isValid(), validB = keyB.isValid();
                     if (!validA && !validB)
                       return a.id < b.id;
                     if (!validA)
                       return false;
                     if (!validB)
                       return true;
                     if (keyA != keyB)
                       return keyA > keyB;
                     return a.id < b.id;
                   });
  if (m_limit > 0 && summaries.size() > m_limit)
    summaries.resize(m_limit);
  const bool complete = candidates.isEmpty() && !scan.truncated;
  if (complete) {
    m_repairSuppressed = false; // 磁盘与缓存一致：恢复自动修复资格
    m_failedCandidates.clear();
  } else if (m_repairSuppressed && candidates != m_failedCandidates) {
    // 候选集合或指纹变化（新保存/外部改动）：上一轮的写失败不是常态，
    // 恢复修复资格；完全相同的失败指纹才持续抑制（防无限重试循环）。
    m_repairSuppressed = false;
    m_failedCandidates.clear();
  }
  emit ready(summaries, complete);
  if (!candidates.isEmpty() && !m_repairTask && m_service && !m_repairSuppressed)
    startRepair(candidates);
}

void AiChatRecentSummaries::startRepair(
    const QVector<paleo::chat::ChatArchiveFileStamp> &candidates) {
  auto run = std::make_shared<RepairRun>();
  PaleoTask *task = m_service->startApplication(
    tr("重建会话列表索引"),
    [candidates, run](PaleoTask *task) -> QString {
      // 池线程 worker：纯文件 IO（有界读 + sidecar 写）；只回填共享
      // RepairRun，不触碰协调器/GUI 对象。每文件之间协作取消。
      const auto outcome = paleo::chat::repairChatArchiveSummaries(
        candidates, [task] { return task->cancelRequested(); });
      QMutexLocker lock(&run->mutex);
      run->items = outcome.items;
      run->vanished = outcome.vanished;
      run->irregular = outcome.irregular;
      return QString();
    },
    PaleoTask::Priority::Low, /*quiet=*/true);
  if (!task)
    return;
  m_run = run;
  m_repairTask = task;
  // #159 排序契约：start() 返回后再连接 finished——池任务终态排队回主线
  // 程，此连接先于任何完成回包生效；finished 单次发射保证不重。协调器先
  // 亡则连接自动断开，worker 由 shared_ptr 护栏安全收尾。
  connect(task, &PaleoTask::finished, this, [this, run, task] {
    m_repairTask.clear();
    if (task->state() == PaleoTask::State::Cancelled) {
      // 用户经任务页显式取消：尊重取消，不自动重启修复——已完成条目已
      // 落盘，剩余待修复状态由下一次显式刷新恢复。
      m_run.reset();
      return;
    }
    QVector<paleo::chat::ChatArchiveRepairItem> items;
    {
      QMutexLocker lock(&run->mutex);
      items = run->items;
    }
    if (run != m_run)
      return; // 非本轮回包（防御；单飞下不应出现）
    m_run.reset();
    bool allWritten = true;
    for (const paleo::chat::ChatArchiveRepairItem &item : items) {
      if (!item.written) {
        allWritten = false; // 缓存写失败：不回填内存，条目保持待修复
        continue;
      }
      m_cache.insert(item.stamp.id,
                     CacheEntry{item.summary, item.stamp.bytes, item.stamp.mtimeMs});
    }
    if (!allWritten) {
      m_repairSuppressed = true; // 持久写失败：停止自动重试，列表保持 partial
      // 记录失败指纹：候选变化前不再自动重试（同一指纹的重试注定同样失败）。
      QVector<paleo::chat::ChatArchiveFileStamp> failed;
      for (const paleo::chat::ChatArchiveRepairItem &item : items)
        if (!item.written)
          failed.append(item.stamp);
      m_failedCandidates = failed;
    }
    refresh(m_limit);            // 重扫热路径（此刻多为命中）→ 发布结果
  });
}
