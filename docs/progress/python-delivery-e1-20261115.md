# E1：Python 内部供货与原生离线安装

日期：2026-11-15；计划见 [Python 交付与 Pi 后续计划](../planning/PYTHON_DELIVERY_AND_PI_AGENT_PLAN.md)。
基线为 #953 合并后的 `e995a477`；首次供货与 CLI 验收源码为 `b46981af`，后续租约
修复冻结于 `affc63e1`（前置修复 `55c483fe`），SDK 保持 `9160615d`。下文分别记录
各源码阶段，不把历史全回归重新标为最终源码结果。本批不启用执行权限，不改既有
业务操作/成果链路，Shell 后置。

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

## 首次供货与安装实际验证（b46981af）

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

## 租约边界修复与最终复验（affc63e1）

后续实际 Windows Qt 6.11.1 验证发现两处租约清理缺陷，均未改变完整性或排他规则：

- `55c483fe`：原锁路径包含 generation 的 `../`；完整核验后若重命名 generation，
  解锁会经过已经消失的目录，遗留物理锁。改为从已核验 canonical generation 的父目录
  构造绝对租约路径，重命名不再改变解锁所需路径。
- `affc63e1`：Qt 6.11.1 的 QLockFile 在普通本地长路径上能够加锁，却可能在析构时
  留下锁文件。独立探针复现后，仅为宿主内部构造的 canonical 本地盘符租约路径添加
  Windows native extended 前缀。调用方原文的 device/UNC 等拒绝规则不变，未启用全局
  longPathAware，也未降低排他性、stale 时间或逐文件 SHA 校验。

两个真实回归分别检查 generation 重命名后的释放/重取，以及超过 260 字符本地路径的
两轮“物理锁存在、并发核验被拒、销毁后物理锁消失、再次获取”。没有用超时删除或
放宽断言掩盖残留锁。

| 阶段与验证 | 默认 | no-ORT |
|---|---|---|
| `55c483fe` 原 focused 集合 | 19/19，37.01 秒 | 20/20，35.02 秒 |
| `55c483fe` Runtime / Delivery QtTest | 26/0/0；23/0/0 | 同左 |
| `affc63e1` 完整目标增量 | paleo、原 50 个 C++ 测试、维护 CLI；另显式构建两个工具 | 同左 |
| `affc63e1` Runtime / Delivery 定向 CTest | 2/2，7.49 秒 | 2/2，8.11 秒 |
| `affc63e1` Runtime / Delivery QtTest | 27/0/0；23/0/0，两个租约回归均 PASS | 同左 |
| 最终 Office / WebEngine 链接 / 应用启动 | 全部通过 | 全部通过 |

两个工具为 `paleo_mkproject` 与 `paleo_data_snapshot_worker`，不计入 C++ 测试数。
日志记录实际 ELF 路径、大小及 SHA；未因 SDK 未变而把旧工具文件直接记为新验证。
两个变更对象的 Ninja 依赖分别为 525/823 项，均 VALID 且包含实际 `pythonruntime.h`。
最终两路顺序各 `-j2`；WSL 的应用检查是 offscreen 首帧与组件/链接检查，不新增
Office 编辑器 DOM 或强隔离执行通过的结论。

继续复用 E1 自己的两个物理 build 与 ext4 源快照，不使用 E3 副本代替 Linux 编译。
原 6,440 文件快照逐文件重新核验，再覆盖并核验两份 C++ 文件；收据明确
`cppSourceHead/buildInputSourceHead=affc63e1`，同时保留 `baseSourceHead=b46981af`，
列出三份未同步文档并标注 `fullGitTreeEquivalent=false`。不将这个复用文件集称为
完整的新 Git archive。

最终 Windows 证据来自 E3 继承同一 `affc63e1` Runtime/Delivery 源码的 Qt 6.11.1
原生 Core 验证壳：4/4、93.59 秒，Runtime **27/0/0**、Delivery **25/0/0**、
Config **17/0/0**、Controller **22/0/0**，共 91/0/0；显式 REQUIRE=1 的可信/生成
两模式与两个租约回归均实际通过。983 个 runtime 文件和八个固定 DLL 执行前后 SHA
一致。此证据仍**不是完整 E1 Windows GUI/QGIS 验收**；其归属与完整应用验证由后续
平台记录说明，不用 Core 结果替代。

独立证据入口（均为 ignored 验证材料）：

- E1 `local/e1-validation/lease-fix-linux/source55/summary.json`：保留 55 阶段原 focused 结果。
- E1 `local/e1-validation/long-path-fix-linux/affc63e1/summary.json`：最终两路 build、
  定向 CTest、所选 Qt 原文、Ninja 依赖、工具 SHA、闭包与启动日志。
- E1 `local/e1-validation/snapshot-affc63e1-long-path-fix-receipt.json`：快照范围与两文件覆盖身份。
- E1 `local/e1-validation/long-path-fix-source-gates/`：最终 fetch/rebase 后的 39 项逐项门禁；
  原 `source-gates/` 与中间 `lease-fix-source-gates/` 不覆盖，基线不增加。
- E3 `local/e3-validation/windows-core-6111/third-18c6-summary.json`、
  `third-18c6-receipt.json` 与四套 Qt 原文：最终 Core 壳证据。独立包
  `third-18c6-qt6111-affc-green.zip` 为 25,077 字节，SHA-256
  `6ecf0a6f4205dce32b0f7f309a3e302eba9ec9d28dea8fa3e8cfbbf6ac8a18be`。

旧脚本的 Qt `-o` 固定写到 `Testing/qtest-first-run`，该目录会随测试更新，不能将其
现内容当作旧源码原件。b469 的 CTest 总日志和汇总保留；旧 LastTest 已另存，55 与
最终阶段的所选 Qt 分文件另行快照，后续生成 CTest 的日志出口也已分开。

## 发现与修正记录

不将修订前的失败计作通过：首次 fd 接管后的路径权限查询为 11/3/0，修成句柄权限；
Unix selftest 清理只读父目录失败，改为仅对本测试拥有的临时树恢复写权限；no-ORT
初始清单引用 ORT-only 目标，在编译前失败后改用实际清单。Windows 22/1 发现 `.leases`
目录枚举差异，24/1 发现 Qt 规范化会消去 device 前缀，均修复后重新编译验证。低空间
夹具的 namespace 身份检查改到降权前，产品代码不因此改变。各轮原始日志保留。

E2–E3 持久配置、启用诊断及 Pi、数据/成果扩展继续按计划推进；本批不宣称那些功能已交付。
