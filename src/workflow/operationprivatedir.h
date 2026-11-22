#pragma once
// 层：功能
//
// workflow/operationprivatedir.h — operation 私有目录的可信解析与创建。
// 单一生产入口：capabilityrecovery_worker.cpp（数值/拷贝 worker 的 privateDir）
// 与 capabilityrecovery_external.cpp（result-<uuid> 附件）共用；任何
// "opId/attempt 直接字符串拼接 + mkpath" 都必须换到这里。
//
// 边界（G4）：
// - 解析永远从规范化的工程根出发，不信任调用方传入的 operationDir 字符串
//   （路径别名共享同一持久身份，见 pathAliasesShareTheDurableIdentity）。
// - 每一段（.paleo / capability-ops / op-… / attempt…）在创建前逐段检查：
//   存在即必须是无符号链接的目录且规范路径仍在工程根内；不存在才允许由
//   mkpath 创建。创建后复核最终目录的规范路径仍精确落在
//   <root>/.paleo/capability-ops/<operationId>/<attempt> 之下——
//   "op-a" 不因字符串前缀关系拿到 "op-ab" 的空间。
// - 校验失败、根不可用、路径越界与实际创建失败使用不同 code，不互相冒充。
// - 已知限制：检查与创建之间（TOCTOU）的外部文件系统竞态无法由本 helper
//   消灭；创建后复核把窗口收敛到"复核之后、worker 使用之前"，残余风险由
//   调用方所在进程的工程写锁边界约束。

#include <QString>

namespace paleo::capability::operation_detail {
struct PrivateDir {
  QString path; // 成功时为绝对路径（'/' 分隔）
  QString code; // 空串 = 成功
};
// leaf 为空 → <root>/.paleo/capability-ops/<operationId>/<attempt>；
// leaf 非空（仅接受 result-<uuid>）→ 追加附件叶子段。
PrivateDir createPrivateAttemptDir(const QString &projectDir, const QString &operationId,
                                   const QString &attemptId, const QString &leaf = {});
}
