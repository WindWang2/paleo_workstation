# 大工件分页完整性与重复 SHA 整改

日期：2026-11-14。按用户要求先合并 #942（`3d499a61`），主 checkout 同步到 master，
再在独立 `codex/operation-read-integrity-20261114` worktree 实施。#942 合入前 fetch/
rebase 并重跑 33 项 Source gates；按仓库约定直接 squash，不等待 CI，随后比对恢复
`lint/linux/windows`、strict=true、enforce_admins=true 及其余原有保护。原未跟踪文件与
六项 stash 保留，已合并工作分支本地/远端清理，旧 worktree 文件与验证日志保留。

## 改动与审查

旧 `CapabilityRecovery::read` 对每页调用整工件 SHA 验证，再重新打开文件取页。
本轮将读取移到独立 workflow TU，以 QtCore 薄库 `AuthenticatedArtifactReader` 实现
同流完整 SHA 锚定的有界块摘要索引。当前页认证、身份、预算、取消、owner 交付与 GC
约束见[契约](../architecture/OPERATION_RESULT_READ_INTEGRITY.md)。

只读交叉审查确认没有以 size/mtime 缓存替代认证，没有发布部分扫描摘要或失败页内容。
审查发现的订阅者线程边界也已收紧：授权复核和最终回调在 recovery owner 上执行，
订阅者用 QPointer 限定交付寿命，完成连接单次释放。

发布/恢复/提交的整工件 SHA 路径继续保留；索引纯内存，复用持久 operation 引用的回收
保护，不新增磁盘格式、临时大副本、长期独占 GC lease 或调用参数。

## 验证

专项薄库已编译并通过 17 项 QtTest 检查，零失败/跳过。32 MiB、1366 页实测：

| 指标 | 字节/次数 |
|---|---:|
| 完整扫描 | 1 次 |
| 完整 SHA | 33,554,432 |
| 实际读取 | 145,358,848 |
| 块 SHA | 145,358,848 |
| 原实现重复完整 SHA（按旧路径推导） | 45,835,354,112 |

两个独立 build 的完整应用已编译通过，相关回归结果：

| 配置 | CTest 套件 | QtTest 通过 / 失败 / 跳过 | CTest 耗时 |
|---|---|---|---|
| 默认 ORT | 46/46 | 731 / 0 / 0 | 211.53 s |
| 无 ORT | 42/42 | 669 / 0 / 0 | 182.37 s |

范围包含分页 reader、Recovery/凭证/恢复、宿主/命令、聊天路由、tile、井输入及 GC。
与 #942 同一子集相比，新增 reader 17 项及 Recovery 9 项；37 项 Recovery 检查在两套
配置均通过。QtTest 总计包含初始化/清理及数据行，跨配置重复用例分别记，不相加。
对编译期间调整过的 Recovery 夹具另做强制重编与复跑，两套均 37/37、零失败/跳过，
避免把旧对象当成最终源码验证。

Windows MSVC 19.38 / Qt 6.8.0 Core/Test 的独立进程也已编译并通过同一 17 项分页测试，
I/O 计数与 Linux 完全相同；只链接 Core/Test 与 CRT，隔离完整应用的 QGIS/Qt 链。
本地化 `/showIncludes` 前缀用编译器原始输出与 Ninja 夹具确定；限定该 native 目标
clean 后以 -j2 重建，17/17 再次通过（CTest 3.06 s）。生产 CPP 的 Ninja 依赖 147 项、
测试 CPP 303 项，均 VALID 且含真实 reader 头，修复前的零依赖日志保留。

`ldd` 核对两个完整应用均无未解析依赖，ORT 链接状态与配置吻合，Office 组件均就绪。
最终 `git fetch` / `git rebase origin/master` 基线为 `3d499a61`，Source gates 33/33
再次通过，包含锁定 QGIS 头包下的四项传递依赖指标，没有增加任何基线。
提交后两配置再次构建完整应用、Recovery 及 reader 目标，均为 `ninja: no work to do`。
两套 build 的 `operation-read-{build,tests,counts,names}` 日志保存实际命令与逐套计数，
`operation-read-final-recovery.log` 保存最后的 Recovery 重编/复跑；源码门禁日志在
`local/review-20261114/source-gates/`，Windows 专项证据在
`local/operation-read-validation/windows-native/`。

环境为 WSL Arch Linux / GCC 16.2.1 / Qt 6.11.2 / CMake 4.4.3，锁定 QGIS 4.2.3 /
GDAL 3.12.2 闭包；默认 ORT 1.30.0，另设禁用 ORT 配置。两路各 -j4 合计不超过 8，
各自 CTest 串行。Office 组件共享主仓并在 configure 确认为就绪。测试使用合成数据、
loopback 端点和真实临时文件；未执行本轮 Windows 完整应用构建、真实工区或人工视觉验收。
