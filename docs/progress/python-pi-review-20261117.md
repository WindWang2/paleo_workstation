# Python、Pi 与类型化输入输出合并后 review

基于合并后的 master `9bc792a9bc857a6723767593e0bb37f69f746c2c`，SDK gitlink
维持 `1362a69e61a663aba2b5024bfa83627af0759354`。本轮检查供货、配置、启用、
诊断、模型流、宿主绑定、取数、成果验收、发布及恢复，沿用现有操作和成果链路。
Shell、任意路径读取、动态包安装与网络权限继续后置。

## 修复

| 问题 | 原行为 | 处理与回归 |
|---|---|---|
| 已取消刷新污染共享缓存 | 旧刷新在新刷新复用已验证 snapshot 后才进入校验，清掉新请求依赖的缓存 | 获取 inspection mutex 后先检查取消；尚未开始校验则保留旧 snapshot。实际校验开始后的取消仍使缓存失效；覆盖两种 inspection、确定的并发交错与后续同尺寸篡改 |
| CSV 展开后超出策略预算 | 二进制数值帧满足预算，但浮点转十进制后的 report＋CSV 可能超限 | 在编码后复核最终总字节数；覆盖 raw frame 已通过、展开后越界及恰好等于预算的边界，拒绝时不返回部分成果 |
| Pi 小分片误触发输出上限 | 每个 delta 均计入完整 JSON 包装，实际合并后只需少数帧 | 统一按 JSON 转义后的文本 payload 计量，继续保留原字符限额和 16 KiB 帧余量；真实 Pi 覆盖 10000 个单字符、同一 owner callback 突发、转义字符与真实溢出 |

## 结构整理与保留项

- 将 A1 握手与 A2 会话重复的最小环境、Windows Job Object、Linux parent-death、
  no-new-privileges 和关闭继承 fd 收进私有 `piprocessplatform_p.h`。各调用方继续
  负责独立运行时认证、协议、启动顺序、超时、取消和回收；旧 Qt/Linux 继续拒绝运行。
- 将数值表和地震道输入重复的 owner/context、正式 Catalog 父子关系、源 revision
  与模型目录核对收进 app 私有 `pythoninputbinding_internal`。格式解析、错误码、
  地震文件完整 SHA 要求、大小约束及 reader 世代绑定留在各 adapter。
- 保留 v1 报告适配与历史恢复、人工 Console、运行时和模板独立 store。它们仍有
  调用方或不同的版本/授权契约，不能仅因名字相近而删除。
- 保留 capture 与 launch 两个信任边界的完整性校验、固定工具集、宿主权限判定、
  分页引用校验、单次发布和 `commit_uncertain` 恢复。Pi 仍为可信编排进程，
  生成 Python 仍走原强后端；本轮不扩大工具能力或提高预算。

不增加 registry、执行队列、通用 adapter 框架或 UI。所有基线保持原值。

## 验证

本节在最终冻结源码完成默认/无 ORT 编译、回归、强后端与供货完整性验收后填写。
旧批次结果仅用于定位可复用缓存，不计为本轮通过。提交 PR 前重新 fetch/rebase
`origin/master`，再运行完整 Source gates。
