// 层：功能
#pragma once
// G1（模型通信）——LLM 配置提交的串行化/补偿协议（applyConfig 的私有协作者）。
//
// 跨存储事实：配置文件（QSaveFile）与系统钥匙串没有跨储 ACID——任意一步
// 失败都可能留下「新端点 + 旧密钥」这类未验证组合，重启后会被当成功启用
// （旧密钥发往新主机 = 凭据外泄面）。本协议以最小 staging/串行/补偿闭环：
//
//   1. 结构校验（地址/模型；密钥单查）——失败即拒，不落任何存储；
//   2. 快照旧配置文件原文 → 写新配置文件（经 LlmConfig::save，原子、不含密钥）；
//   3. 密钥操作（newKey 写 / clearKey 删 / 沿用不动）——异步，串行链：
//      同一时间至多一个密钥操作在途，后继提交排队等待，实际存储次序恒等于
//      提交次序（并发提交不可能由较旧回调覆盖较新生效组合）；
//   4. 密钥失败 → 补偿：还原旧文件原文；此前未配置（无旧文件）或还原失败
//      → 清钥匙串密钥（重启即诚实禁用态，须重新配置）；
//   5. 清钥匙串也失败 → 如实报错并点名须人工重新配置——绝不把明文密钥写进
//      文件/日志/历史来自救。
//
// 语义保留（与旧 applyConfig 一致）：newKey 优先（clear 冗余忽略）、
// clearKey 且无新密钥 = 清除、都无 = 沿用当前密钥、允许无密钥保存禁用配置。
// 密钥操作回调以本对象为生命周期锚（协议链不因对话框关闭而断链），用户
// done 回调仍按调用方给的 callbackContext 守卫（context 先亡则静默丢弃）。
#include "llmclient.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>

// 可注入存储面（确定性测试用；生产用 defaultLlmConfigCommitHooks()）。
struct LlmConfigCommitHooks {
  // 读当前配置文件原文（不存在/不可读 → 空串）——补偿还原的快照来源。
  std::function<QByteArray()> readFile;
  // 按既有序列化口径原子写新配置（生产 = LlmConfig::save：形状单一来源）。
  std::function<bool(const LlmConfig &form, QString *error)> writeConfig;
  // 原子写回快照原文（补偿路径专用：字节级还原，不经序列化）。
  std::function<bool(const QByteArray &bytes, QString *error)> writeFileBytes;
  // 异步写密钥（context 保证回调生命周期；回调必达，即使失败）。
  std::function<void(QObject *context, const QByteArray &key,
                     std::function<void(bool ok, const QString &error)>)>
    writeKey;
  // 异步清密钥（幂等：条目不存在也算清成功）。
  std::function<void(QObject *context,
                     std::function<void(bool ok, const QString &error)>)>
    clearKey;
};

// 生产实现：LlmConfig::path()/save() + QSaveFile 原文还原 + LlmKeyStore。
LlmConfigCommitHooks defaultLlmConfigCommitHooks();

class LlmConfigCommitter : public QObject {
  Q_OBJECT
public:
  explicit LlmConfigCommitter(QObject *parent = nullptr);
  ~LlmConfigCommitter() override;

  void setHooks(const LlmConfigCommitHooks &hooks) { m_hooks = hooks; }

  // 串行提交一次配置。done(ok, error) 在 callbackContext 仍存活时回调；
  // apply 仅在提交成功时调用一次（生效走调用方注入的现有 setConfig 通道，
  // 先 apply 后 done——ok 时配置已生效）。
  void submit(const LlmConfig &form, const QByteArray &newKey, bool clearKey,
              const LlmConfig &current, QObject *callbackContext,
              std::function<void(bool ok, const QString &error)> done,
              std::function<void(const LlmConfig &applied)> apply);

private:
  struct Pending {
    LlmConfig form;
    QByteArray newKey;
    bool clearKey = false;
    LlmConfig current; // 提交时的生效配置（沿用密钥的来源）
    QPointer<QObject> callbackContext;
    std::function<void(bool, const QString &)> done;
    std::function<void(const LlmConfig &)> apply;
  };

  void pump();
  void runHead();
  // 收口：出队 → （成功时）apply → （context 存活时）done → 推进链。
  // applied 为空 = 失败提交（不生效）。
  void completeHead(bool ok, const QString &error, Pending head,
                    const LlmConfig *applied);
  // 密钥操作失败后的补偿：还原旧原文；无旧文件/还原失败 → 清钥匙串。
  void compensateAndComplete(const QByteArray &previousBytes,
                             const QString &cause, Pending head);
  // 密钥操作完成后的公共推进（成功生效 / 失败走补偿）。
  void onKeyOpFinished(const QByteArray &previousBytes, Pending head, bool ok,
                       const QString &keyError, bool wasClear);

  LlmConfigCommitHooks m_hooks;
  QList<Pending> m_queue;
  bool m_inFlight = false; // 密钥操作在途（串行链的唯一闸）
};
