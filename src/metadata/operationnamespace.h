#pragma once
// 层：数据
//
// metadata/operationnamespace.h — operation 私有命名空间的最小身份事实。
// 纯数据验证：不建目录、不读文件、不改日志。workflow（capabilityops/
// capabilityrecovery/capabilitypublication）与 services（operationretention）
// 共用同一判断，避免各处自拼前缀。
//
// 历史事实（生成器与已发布 fixture，2026-11 核对）：
// - operationId 恒为 "op-" + QUuid::WithoutBraces（capabilityops.cpp accept()、
//   capabilityoperationcontrol.cpp acceptDurable()、restoreSeismicTraceResult
//   已按此形态校验）。路径消费：.paleo/capability-ops/<op>/<attempt> 与
//   <operationDir>/results/<opId>-*（v1 遗留）。非该形态的记录一律不入
//   私有目录命名空间。
// - ownerAttempt 两种合法形态：生产裸 UUID（capabilityrecovery_worker.cpp、
//   capabilityrecovery_external.cpp）与已发布控制测试 fixture 的
//   "attempt-" + UUID（tests/tst_operation_control.cpp）。两者都保留；
//   不强制归一，不改历史记录。
// - remoteJobId、callerScope、documentId 等不参与路径组成，不套用本契约。

#include <QString>
#include <QStringView>

namespace paleo::capability::operation_namespace
{
// "op-" + 小写 UUID（36 字符），共 39 字符。与全部现有生成器逐字节一致。
bool isCanonicalOperationId(QStringView id);
// 裸 UUID（36）或 "attempt-" + UUID（44）。覆盖生产与已发布 fixture。
bool isCanonicalAttemptId(QStringView id);
// "result-" + UUID：远端结果附件叶子目录（capabilityrecovery_external.cpp）。
bool isCanonicalResultLeaf(QStringView leaf);
// 工程相对私有命名空间前缀 ".paleo/capability-ops/<operationId>/"。
// id 非规范形态时返回空串——调用方不得把非规范 id 拼进任何路径。
QString privateOperationPrefix(const QString &operationId);
// relative 是否落在给定 operation 的私有命名空间内（精确到段，
// "op-a" 不因前缀字符串关系认领 "op-ab" 的成员）。不做文件系统检查。
bool isInPrivateOperationNamespace(QStringView relative, QStringView operationId);
}
