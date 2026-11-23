// 层：功能
#pragma once
// H2（agent-session-archive，2026-11-22）：最近会话列表的可重建元信息索引。
//
// 纪律（W4）：会话 JSON 是唯一权威历史；本索引只保存列表所需的
// sessionId/显示标题/model/时间与最小文件新鲜度戳，是**可丢弃重建的派生
// 缓存**——缺失、损坏、版本不同或被外部改动时丢弃重建，永不反向覆盖、
// 删除或「修复」正文。索引项绑定受校验的文件身份（sidecar id 必须等于
// 文件名 id），实际打开仍走 ChatSessionStore::load 的权威校验；本索引
// 不提供工具资格或 operation 权限（metadata 不是授权）。
//
// 形态：per-session sidecar（<会话根>/index/<id>.meta.json），写入成本与
// 会话总数解耦——checkpoint 只重写被保存会话自己的小文件，不再维护一份
// 随全库线性增长的总索引。新鲜度用 size+mtime 弱指纹（不重 hash 正文），
// 只做缓存判断，不当作防篡改证明。
#include "chatsession.h"
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace paleo::chat {

// 列表用的会话摘要（不含正文/工具实参/凭据/host 绑定明细）。
struct ChatSessionSummary {
  enum class State { Ok, OverLimit, Invalid };
  QString id;             // 权威文件名 id（打开/删除都以它为准，非缓存自述）
  QString displayTitle;   // ChatSession::displayTitle 同语义（缓存侧有界截取）
  QString model;
  QDateTime createdAt;
  QDateTime updatedAt;
  State state = State::Ok;
  QString diagnostic;     // 非 Ok 时的稳定英文码（archive_size_exceeded 等）
  qint64 fileBytes = 0;   // 诊断辅助；排序/身份一律用会话自身时间戳
};

// 权威文件的弱指纹（缓存新鲜度判断专用）。
struct ChatArchiveFileStamp {
  QString id;
  qint64 bytes = 0;
  qint64 mtimeMs = 0;
  bool operator==(const ChatArchiveFileStamp &other) const {
    return id == other.id && bytes == other.bytes && mtimeMs == other.mtimeMs;
  }
};

struct ChatArchiveScan {
  QVector<ChatArchiveFileStamp> files; // (mtimeMs desc, id asc) 确定序
  bool truncated = false;              // 超 maxEntries 截断：列表只能声明 partial
  int skippedIrregular = 0;            // 符号链接/目录伪装/不安全名：跳过不读
};

// 有界扫描会话根：只 stat、不读正文。非常规叶子（符号链接等）计数跳过。
ChatArchiveScan scanChatArchiveFiles(const QString &root, int maxEntries);

class ChatSessionIndex {
public:
  // sidecar 载入后的内存形态。
  struct Entry {
    QString id;
    QString displayTitle;
    QString model;
    QString diagnostic;
    QDateTime createdAt;
    QDateTime updatedAt;
    ChatSessionSummary::State state = ChatSessionSummary::State::Ok;
    qint64 sourceBytes = 0;
    qint64 sourceMtimeMs = 0;
    ChatSessionSummary toSummary() const;
  };

  static QString indexPath(); // <会话根>/index
  // sidecar 路径；id 非单段安全名时返回空串（与 pathFor 同边界）。
  static QString sidecarPathFor(const QString &sessionId);
  // 原子写 sidecar（QSaveFile）。调用方传入权威文件的新鲜度戳；失败返回
  // false——**缓存写失败不影响权威保存事实**，条目留给下一次修复补齐。
  static bool write(const ChatSessionSummary &summary, qint64 sourceBytes,
                    qint64 sourceMtimeMs);
  // 有界读取 + 形状校验（schema=1、id 与文件名一致、字段类型）。任何不合
  // 格都返回 false（视为待修复），不抛错、不改正文。
  static bool read(const QString &sessionId, Entry *out);
  // 权威文件删除后的缓存清理（best-effort；失败仅留孤儿，扫描期 prune）。
  static void removeFor(const QString &sessionId);
  // 清理没有权威文件的孤儿 sidecar（只动缓存目录，永不碰正文）。
  static void pruneOrphans(const QStringList &liveIds);
  // 由合法载入的会话构造列表摘要（displayTitle 语义与正文侧一致）。
  static ChatSessionSummary summarize(const ChatSession &session, qint64 fileBytes);
};

struct ChatArchiveRepairItem {
  ChatArchiveFileStamp stamp;      // 修复时的权威文件指纹（缓存回填用）
  ChatSessionSummary summary;
  bool written = false;            // sidecar 是否成功落盘
};

struct ChatArchiveRepairOutcome {
  QVector<ChatArchiveRepairItem> items;
  int vanished = 0;   // 候选在修复时已不存在（缓存随之清理）
  int irregular = 0;  // 候选变成目录/链接等非常规形态：跳过不读不缓存
  int changed = 0;    // 修复读取期间权威文件被改写：不写缓存，留给下一次修复
};

// 冷修复：对候选逐个重建 sidecar。可在任意线程运行（纯文件 IO，不触碰
// GUI/工程/RAW/operation 面）；每文件之间经 shouldStop 协作取消（返回
// true = 停止，如 task->cancelRequested()）。超上限旧档案只产 OverLimit
// 元信息（不读正文、不伪称完整载入）。
ChatArchiveRepairOutcome repairChatArchiveSummaries(
    const QVector<ChatArchiveFileStamp> &candidates,
    const std::function<bool()> &shouldStop);

} // namespace paleo::chat
