# PY3W：Windows 受控 Python 隔离后端

本批只提供宿主内部隔离后端及状态探测；生成代码工具在 PY4 接线。
Shell、在线安装、任意可执行文件/argv、交互解释器与模型选择 backend 均不在范围内。

## 边界与所有权

`SandboxRequest` 只接宿主保留的 `VerifiedRuntime`、`InputBundle`、执行策略、取消器和
全局执行票据。`allowGenerated` 必须明确开启；仅 `allowTrusted` 不授予生成代码执行权。
普通可信进程和强隔离进程复用 PY1 的同一全局并发票据、Job/管道/进程回收原语。
强后端始终传完整 AppContainer 身份，准备失败不回退普通进程。

运行时和输入都先取得拒绝共享写入/删除的文件及目录句柄，再重新核验完整清单和每文件
SHA。运行时使用宿主冻结的 manifest SHA 和已有 retention；不接受目录自报摘要。
输入重验已有 `reference.manifestHash`，认证的同一字节用于 stdin。拒绝 UNC、别名根、
reparse/junction、额外硬链接、设备及超预算树。句柄保持到子进程回收结束，避免核验后
代码、原生扩展或输入被原地覆盖、删除或目录替换。

只执行认证输入中的 `code.py`，解释器选项固定为 `-I -u -B -X utf8`。
`metadata.inputProtocol` 只允许以下两种精确文件集；JSON 必须为对象且不超过 64 KiB：

| 协议 | 文件集 | stdin |
|---|---|---|
| `paleo.python-json.v1` | `code.py`, `request.json` | 认证 JSON 字节 |
| `paleo.python-analysis-frame.v1` | 上述两文件及 `data.bin` | uint32LE JSON 长度、JSON、binary |

框架的数据轴、dtype、单位、质量与来源语义由 PY2 宿主校验；后端不选择任意输入文件，
也不提供自由 stdin/路径/环境变量覆盖入口。framed 协议的 `inputBytes` 包括 4 字节
长度头、JSON 和 binary；4096 字节精确边界通过，payload 已达 4096 而帧总长为
4100 字节时在启动进程前拒绝。修复前的真实失败与修复后的回归日志均保留。

## OS 强制与资源口径

每次使用独立 UUID AppContainer profile，启用 LPAC，只有系统 `registryRead` 和私有
运行时只读 capability。运行时保护 ACL 只授予 capability 读/执行；输入只授予本任务
package SID 读/执行。profile 的真实文件基目录和私有注册表子树均替换为保护白名单，
包括已有子项；OWNER_RIGHTS 仅保留 READ_CONTROL，避免所有者获得写 DACL 权。
单纯 cwd、venv、Job 或给默认 ACL 加一条 deny 均不构成本后端。

子进程没有可写磁盘或注册表工作区，没有网络/loopback capability，也没有宿主 IPC、
项目、凭据及其他任务工作区的访问授权。临时目录环境值指向只读输入。环境仅含系统目录
和数值库线程配置，不继承宿主密钥。结果由有界 stdout broker 交宿主，stderr 单独限额；
未通过领域格式、SHA、端口及发布授权校验的字节没有成果权限。

PY1 公共 runner 通过原子 Job-list 和显式 stdio handle-list 创建进程，禁止 breakaway，
Job 进程数硬上限为 1，宿主退出关闭 Job 即回收子进程。Job 强制总 committed memory 和
一核等效 CPU hard cap；用户态每 10 ms 读取 user+kernel 累计 CPU，并有 OS CPU 限额
作后备。超时、取消、内存/CPU/输出/日志/进程超限均清空数值 output。失败 stdout 最多
保留 `logBytes` 为私有 `outputLog`，不会成为数值端口。

`threads=1` 是数值库线程配置及默认计算并行度，Windows 不宣称独立线程数量硬上限。
readiness 显式返回 `threadCountEnforced=false`、`cpuParallelismLimit=1`；生成多线程仍
共用同一 Job 的 CPU 总额和内存预算。线程 API 并未被禁止，模型也不能调大策略。

analysis-core 的 child committed memory 上限仍为 512 MiB；Job 完整峰值计数成功且进程
已退出后才置 `memoryPeakComplete=true`。宿主另有不高于同值的缓冲保留预算：固定
64 MiB 清单/路径/句柄等元数据余量、`3*(outputBytes+logBytes)` 的重分配余量，以及实际
认证 stdin 字节和 8192 字节开销；超额提前返回 `python_snapshot_memory_budget`。
运行时和输入本体通过文件锁固定，不复制为宿主内存。`hostSnapshotReservedBytes` 和
`childMemoryLimitBytes` 与 child 峰值分别报告；这些字段不声称 GUI 总 RSS 有 512 MiB 上限。

磁盘方案采用无子进程写权限加宿主有界结果 broker，未部署卷配额，也不以轮询目录大小
充当硬限额。输入加输出/日志保留须不超过 policy.diskBytes；profile/恢复日志为宿主固定
控制元数据，不是脚本可写的扩容通道。

## 生命周期与失效

profile 创建前在仅宿主/SYSTEM 可写的控制目录登记 moniker、host PID 和精确创建时间。
正常退出立即删 profile；崩溃后只处理本服务格式、严格 UUID 文件名及身份匹配的记录，
确认原 host 已退出或 PID 已复用才删对应 profile。不扫描或删除其他 AppContainer。
API/OS、清单、ACL、文件锁、profile、Job 或回收缺项均失败关闭，不提权、不改全机策略。
readiness 本身只读，不创建 profile、运行进程或触发清理；实际准备每次重新检查。

## 原生验证与支持范围

目标实测环境为 Windows 11 Pro x64 build 26200、普通用户、MSVC 2022 14.38。
运行时 CPython 3.14.10 + NumPy 2.5.4，982 个 regular 文件、66,435,629 payload 字节：

- source lock SHA：`109899e2011cde44e1ed7cab28a962af7e0273ee9eaab38cec34c44da8554014`
- runtime manifest SHA：`94446a0c28dc1f2b2b8541f8e0151e964aff5b7ab36f2868742fbd553dc665ad`

测试使用供货 generation 的私有完整副本，ACL 与篡改恢复不触碰共享供货目录。
旧清单绑定旧 source lock 被严格拒绝为 `python_manifest_invalid`；未硬编码旧锁绕过。

Windows 测试目标 `tst_python_windows_sandbox` 使用 `add_paleo_test` 注册、串行执行。
完整原生矩阵要求以下宿主环境值；必需模式缺配置直接失败，不能 QSKIP 取得平台准入：

```powershell
$env:PALEO_TEST_AGENT_PYTHON_ROOT = '<private verified generation>'
$env:PALEO_TEST_AGENT_PYTHON_MANIFEST_SHA256 = '<trusted manifest SHA>'
$env:PALEO_REQUIRE_AGENT_PYTHON_SANDBOX = '1'
ctest --test-dir build -R '^tst_python_windows_sandbox$' --output-on-failure -j1
```

没有配置供货包的默认安装只运行明确标注的 unavailable 行为测试，不能把它当隔离矩阵。
Qt 6.8.0 Core/Test 独立 native 壳验证 Windows 强后端，没有加载 QGIS。本机没有匹配
Qt 6.11.2 的 Windows QGIS 开发闭包，完整应用出口使用下述真实 Linux 车道；不据此
声称 Windows 完整 QGIS 应用或未测试 Windows 版本/文件系统已取得部署支持。

原生中间证据在该 worktree 的 `local/py3w-validation/`（gitignored）：首轮 16/16，
0 fail/skip；扩展夹具失败日志保留。取消 callback 中同步遍历进程造成准备时间膨胀已改为
独立稀疏 monitor；SDK 存在的 LPAC token 查询枚举在本机未获支持，改用 AAP-only 读权限
哨兵的真实拒绝测试，不把枚举存在当成功。活动 child 期间清除 RO 属性后，runtime/code
写删仍得到 sharing violation，项目祖先目录替换也被拒，定向测试 3/3、0 fail/skip。
最终 native 全矩阵 22/22，0 failed/0 skipped，199.068 秒；缺环境单独运行
unavailable 行为测试 3/3，0 failed/0 skipped，未将其计入平台隔离证据。
当前基底为 PY2 #948 `17317b1b`，SDK 为 `9d80d1f6`，包含最终公共进程 UTF-8
边界修复；公共进程原语未在本批复制或分叉。最终 rebase 只继承文档变化和已具备的
registry 成员测试，`src/`、`tests/`、`cmake/` 与验证快照 `5675d50d` 的 diff 为空。

| 实测 | 数值 |
|---|---:|
| NumPy dot 任务 user+kernel CPU | 781,250 µs |
| NumPy child committed peak（完整 Job counter） | 26,206,208 B |
| NumPy 宿主保留预算 | 271,589,388 B |
| 1 秒 CPU 限额实际累计 CPU | 1,015,625 µs |
| 4 个 native NumPy 线程的同一限额实际累计 CPU | 1,078,125 µs |

CPU 阈值测试容许 0.5 秒 OS/轮询误差，当前最大实测超额 78.125 ms；此容差没有放大
policy 或 Job 限额。内存用例真实申请 1 GiB 触发 Job memory-limit，写盘耗尽用例尝试
1024 次写入均失败。host 文件/注册表/跨工作区哨兵保持原字节，监听服务没有连接，
硬链接/junction 拒绝没有改变目标，宿主崩溃后的 child 已退出且 profile journal 可核对清理。

独立壳的精确重跑入口为本 worktree
`local/py3w-validation/run-native.cmd`（MSVC、Qt Core/Test 6.8.0、`-j1`）；完整应用测试
使用上面的正式 CMake 目标。`configure.log`、`build.log`、`tests.log`、
`build/sandbox-qtest.txt`、`missing-final-qtest.txt`、失败前例和 `native-deps.txt` 均保留。
实际 Ninja 依赖记录已核对：sandbox 四个生产 TU 分别为 155/155/162/164，公共 Windows
process 三个 TU 为 152/148/149，均 VALID，实际本地化 include-prefix 夹具成功。
这些记录避免把错误的 `/showIncludes` 前缀下旧对象误当本轮增量验证。

当前 CI Source gates 加 qgis-spread selftest、known-reds validate/selftest、atomic-kernels
共 37 项（含模板供货两项）全部通过；最终 rebase 后使用同一驱动复核，日志
`local/review-20261114/source-gates/results.json`。显式
`QGIS_PREFIX=C:/Users/wangj.KEVIN/projects/paleo-fusion/artifacts/fusion-validation/qgis/usr`
解析完整外部头，transitive 四指标均参与；功能 QtWidgets 边为 0，视图边记录 4，heavy
为 36 重头/41 TU。file-budget 51 项基线无新增、回升、可收紧或可收缩，所有基线未改。
默认/no-ORT 的 Qt 6.11.2 / GCC 16.2.1 完整应用、42 个 C++ 测试目标及实际夹具依赖
`paleo_mkproject`、`paleo_data_snapshot_worker` 已分别编译完成；Office 与 WebEngine
闭包完整。WSL 没有 CPU controller 委派，未把该环境的真实执行缺项当作通过。
完整回归在独立普通用户、capabilities 全空且 CPU/memory/pids 委派齐备的真实 Linux
7.2.11 内核 VM 中串行运行，两种配置首轮均通过：

| 配置 | CTest suites | Qt 通过 / 失败 / 跳过 | CTest 时间 |
|---|---:|---:|---:|
| 默认 | 46/46 | 616 / 0 / 0 | 508.68 s |
| no-ORT | 46/46 | 610 / 0 / 0 | 383.83 s |

每组为 42 个 C++ 目标加运行时与模板供货的 4 项检查。真实 Host、Recovery、Agent
查询/调用、SDK 输入、报告、分页与恢复链均在本批源码上执行；应用首帧和 Office
入口成功。app、Qt Core、Qt Gui、WebEngine、QGIS、GDAL 六组 `ldd -r` 全部解析，
driver 没有关闭 WebEngine sandbox。Windows 强后端的 OS 准入另以上述原生 22 项为准。

VM 包的 10,767 个文件共 3,034,385,696 字节，全部以宿主外部锚定的清单
`5807aa2e0122c2afa852499819ac15a4479081fb3521d33857c77358d950dfe0` 核对 SHA。
两组分别持有全新只读运行时副本，执行后重新核对清单锚、2143 个文件的 SHA/大小、
完整文件集和权限，通过且未新增 pyc。原有受调试器污染的目录留作失败证据，未复用。
完整记录在 `local/py3w-validation/native-summary.json` 及 VM 的每组
`py3w-native-{tests,identity,closure,abi-*,smoke}.log`、`py3w-native-post.json`。
