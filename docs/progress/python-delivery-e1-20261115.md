# E1：Python 内部供货与原生离线安装

日期：2026-11-15；计划见 [Python 交付与 Pi 后续计划](../planning/PYTHON_DELIVERY_AND_PI_AGENT_PLAN.md)。
基线为 #953 合并后的 `e995a477`；最终执行验收源码冻结于 `b46981af`，SDK 保持
`9160615d`。后续只补验收文档。本批不启用执行权限，不改既有业务操作/成果链路，Shell 后置。

## 交付与边界

- 供货端复用已准备 generation，新增顺序流封装；原始 runtime/scripts manifest 身份不变。
  外部整包 SHA 与逐文件 SHA 双重核验，64 KiB 流式复制，拒绝越界、别名、重复、截断、
  超限和额外条目。协议与操作说明见 [DELIVERY](../../vendor/agent-python/DELIVERY.md)。
- 客户端为 C++/Qt Core 的 `paleo_python_environment` 及同一安装服务，无系统 Python、
  pip、解包程序、网络或解释器执行依赖。可选 target/安装组件均不进入默认构建/安装，
  不注册为 Agent 工具。
- 安装只接受专用 runtime 或 scripts store，与现有 preparer 共用 OS 锁。完整核验后
  同文件系统发布不可变 generation，再原子更新安装选择；不编辑或删除旧 generation。
  取消和提交前失败保留旧选择，提交后目录同步不确定单独报 `commit_uncertain`，不伪称回滚。
- 普通本地 Windows 长路径不依赖进程 longPathAware；先验证调用方原文，再做路径规范化，
  拒绝 device/UNC/ADS、junction 与硬链接。陌生目录及混合 store 均拒绝且不写入。
- 包用于已授权的内部准备、安装和验证；模板仍为 `notice-only`。存在 LICENSE 文本也
  不被推断为分发授权，当前 `--purpose redistribute` 对两类包均拒绝。外部发行许可确认
  仍是实际对外供货条件，本批没有授予或声称取得该授权。

## 最终实际验证

Linux 默认和 no-ORT 各独立物理 build；私有 mount namespace 复用已核验逻辑路径和缓存，
源码原始字节/mtime 导出到 ext4 并逐文件 SHA 核对。原始 worktree、旧物理 build 和其他
VM 未修改；编译主路 `-j3`，SDK 配套编译独立 `-j2`，资源测试独占。

| 验收 | 默认 | no-ORT |
|---|---|---|
| 完整编译 | paleo、50 个 C++ 测试目标、维护 CLI | 同左，采用真实 no-ORT 目标清单 |
| focused CTest | 19/19，29.48 秒 | 20/20，26.70 秒 |
| 新安装器 QtTest | 23/0/0 | 23/0/0 |
| WebEngine 实际链接 / Office / 应用首帧 | 全部通过 | 全部通过 |

Windows 原生 MSVC/Qt Core 实际编译与 CTest 通过；安装器 **25/0/0**，包含普通长路径、
caller device/UNC 负例和完整 generation 目录集合。这是原生部署组件证据，**不是完整
Windows GUI/QGIS 构建**。当前 WSL 不具备生成执行所需控制器委派；本批不新增强执行
平台承诺，既有支持范围仍按 [支持表](../../vendor/agent-python/SANDBOX_SUPPORT.md)读取。

两平台分别完成真实供货包到新 store 的原生 CLI 验证，各 10 条记录：

| 平台 | 逐 SHA 核对的实际文件（含 manifest） | 客户端 PATH |
|---|---|---|
| Linux | runtime 2144；scripts 5 | 不存在的 Python-free 路径 |
| Windows | runtime 983；scripts 5 | 仅 Qt Core DLL 目录与 System32 |

复装保留同一身份；Python→C++ 与 C++→Python 互斥、正常释放和进程死亡后释放全部通过。
Windows 另有 11 条真实 junction/hardlink/device/UNC/ADS/relative-package 检查及两处
原生 junction 夹具，sentinel 和旧选择不变。客户端未启动 Python 或执行模板。

低空间验收位于私有 namespace 的 8 MiB tmpfs：先成功安装，再填充至剩余 3,108,864
字节；重试返回 `python_delivery_space_insufficient`，旧选择不变且无 stage 残留。

源码门禁使用现行 CI Source gates 35 项加补充自检 4 项，包含显式锁版 QGIS_PREFIX 的
transitive、严格 layering/file_budget、供货/安装自检、C++ 主线及文档检查；开 PR 前
fetch/rebase 后整套重跑，最终退出码和逐项结果保留在本批 PR 与 ignored 验证日志中。
基线未放宽。原始汇总入口为本 worktree 的 `local/e1-validation/final-summary.json`。

## 发现与修正记录

不将修订前的失败计作通过：首次 fd 接管后的路径权限查询为 11/3/0，修成句柄权限；
Unix selftest 清理只读父目录失败，改为仅对本测试拥有的临时树恢复写权限；no-ORT
初始清单引用 ORT-only 目标，在编译前失败后改用实际清单。Windows 22/1 发现 `.leases`
目录枚举差异，24/1 发现 Qt 规范化会消去 device 前缀，均修复后重新编译验证。低空间
夹具的 namespace 身份检查改到降权前，产品代码不因此改变。各轮原始日志保留。

E2–E3 持久配置、启用诊断及 Pi、数据/成果扩展继续按计划推进；本批不宣称那些功能已交付。
