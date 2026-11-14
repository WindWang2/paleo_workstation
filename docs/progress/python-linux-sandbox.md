# 受控 Python：Linux 强隔离后端

本批依赖 PY1 的共同进程泵、全局单任务 permit、cgroup 统计和进程树回收。
强入口不调用可信模板的宽权限准备函数；任何必需能力缺失都返回不可用或失败。
Qt < 6.7 由 CMake 选择不可用实现，不降低全项目 Qt 最低版本。

## 执行边界

宿主只传入已冻结的 `VerifiedRuntime`、`InputBundle`、策略和取消回调。
后端固定运行隔离根下的 `/input/code.py`，不接受程序路径、参数或环境覆盖。
只接受显式声明的两种输入协议：

| 协议 | 文件集（另含 manifest.json） | 标准输入 |
|---|---|---|
| paleo.python-json.v1 | code.py、request.json | request.json |
| paleo.python-analysis-frame.v1 | code.py、request.json、data.bin | 4 字节小端 JSON 长度、JSON、data.bin |

`request.json` 最多 64 KiB。输入 manifest 必须匹配宿主 reference 的
`manifestHash`；运行时 manifest 必须匹配宿主 `VerifiedRuntime.manifestSha256`。
JSON 必须是严格 UTF-8 的 object；JSON 与 data.bin 独立合计不超过 inputBytes，
不能借用未使用的 codeBytes。输入/代码/manifest 与 output/log 的 broker
保留量另受 diskBytes 限制；运行时私有 tmpfs 计入内存。
运行时 generation 的 retention 在整个快照与执行阶段保留。

只读目录绑定不足以防住宿主原地改写。本实现对 manifest 中的每个普通文件
用同一批 64 KiB 缓冲同时计算 SHA-256 和写入 memfd，比较预期 SHA 后要求
WRITE/GROW/SHRINK/SEAL 四个 seal 均生效。读取经 `openat2` 限制在已打开根
目录内，拒绝 symlink。stdin 也从认证后的 sealed FD 取数，不重开源路径。
取消、校验失败和执行结束均通过 RAII 释放所有 FD。

当前固定 bubblewrap 对匿名 memfd 的 `--ro-bind-fd` 不能成功解析源路径，
因此使用 `--ro-bind-data` 将密封内容复制到子进程 cgroup 内的私有 tmpfs，
再只读挂载。脚本看不到宿主可写目录。运行时和输入之外只挂载锁定的最小
系统 ELF 闭包；宿主 `/home`、工程目录、user bus 和网络不可达。

要求 user/mount/PID/network/IPC/UTS/cgroup namespaces、no-new-privileges、
清空 capabilities、禁止再建 user namespace，以及密封 seccomp 过滤器。
过滤器禁止网络 socket、创建进程/线程、切换 namespace、ptrace、跨进程内存、
keyring、io_uring、BPF、挂载及文件句柄逃逸等入口。NumPy 固定单线程。

## 资源与依赖

必需辅助程序和其系统库闭包沿用 PY1 的内容 SHA 锁；检查固定路径、规范路径、
root 所有权、父目录不可被普通用户写入、无 setuid/setgid/file capability。
不搜索 PATH，不从环境选择替代 backend，不在运行时安装依赖。
当前锁包含实际验证的两个 Arch 栈；其他栈需要显式供应、锁定和验收。

子进程 cgroup 的 `memory.max` 仍为策略值（默认 512 MiB），交换为零、
TasksMax 为 8、CPUQuota 为单核 100%。释放执行 barrier 前逐项读取实际
`memory.max`、`memory.swap.max`、`pids.max`、`cpu.max`，缺项、无界或放宽
均拒绝执行。CPU 硬限额和 wall timer 覆盖 bubblewrap 准备阶段。
后端取得或接收 permit 后即开始总 wall deadline，校验、密封和辅助准备均计时，
同一个期限取消回调传入共同进程泵；超时返回 timeout，保持用户取消优先。

宿主数据有另一个不超过策略内存值且不超过 512 MiB 的保留预算，包含：

- 完整运行时和输入 memfd 的字节数；
- stdin 的独立缓冲（frame 分块传递，不再拼接一份大帧）；
- 32 MiB 有界 manifest/路径/FD 元数据余量；
- `3 * (outputBytes + logBytes)` 的保守 broker 容量，覆盖 Qt 容器增长与交接余量。

子进程 tmpfs 总量另须在内存上限内留下 64 MiB 解释器工作余量；过大的组合
在执行前返回 `python_snapshot_memory_budget`。这些数值区分宿主保留预算和
子进程 OS 峰值，不表示整个 GUI RSS 总计 512 MiB。

默认预算固定保留 32 + 3 × (64 + 1) = 227 MiB；加上该运行时约
237.5 MiB，留给 input memfd 与 stdin 副本约 47.5 MiB。frame 的大型
data.bin 被保存和转发各一份，因此默认实际单个数据体上限约 23.7 MiB，
还须扣除代码、JSON 和 manifest。`inputBytes=256 MiB` 是单项上限，
并不保证此组合能在总体保留预算内执行；各个限制均不放宽。

最多 4,000 个快照文件、4,096 FD、8,750 个辅助参数和 1 MiB 参数数据。
只在当前进程 hard limit 允许时把 soft RLIMIT_NOFILE 提至 4,096；不改全机
配置。强子进程另把 soft/hard FD 限额固定为 4,096。
每次执行重新认证并密封内容，没有按路径/时间戳命中的校验缓存。
stdout/stderr 由共同有界 broker 接收；失败时清空数值结果，并把 stdout 的
前 `logBytes` 字节保留为私有 `outputLog` 诊断，不能作为计算成果发布。
共同进程泵以 cgroup 累积 CPU 用量核对整个进程树（含准备阶段）的 CPU
预算，20 ms 轮询超限后回收；同时保留单进程 RLIMIT_CPU 内核硬限与单核 quota。
Linux 的 `peakMemoryBytes` 是实际观测峰值，`memoryPeakComplete=false`。

## 验证口径

WSL 的证据与非 WSL native Linux guest 分开记录。固定运行时为
CPython 3.14.10、NumPy 2.5.4，共 2,143 个普通文件、249,075,081 字节。
WSL 使用 Microsoft 6.6.87.2 内核；独立 QEMU/KVM guest 使用 Arch
7.2.11-arch1-1 内核，普通 UID 1000，无宿主全局安装、提权或策略变更。

快照单测覆盖内容 seal、宿主预期 manifest 锚、同长度改写、symlink、部分失败
FD 释放、输入 frame、协议/预算拒绝和实际 controller 限额的坏值/缺项。
真实完整探针覆盖 NumPy、4 MiB 双向 IO、宿主文件/网络/SysV IPC/FD 隔离、
CPU/OOM/wall、输出/日志超限、取消，以及密封后原地改写代码和 NumPy 文件。
另在 Python worker 已真正 exec 后杀住宿主，核对后代回收与实际限额。

每个文件的私有 tmpfs 复制和挂载有可见开销；当前 WSL 完整任务约 9–13 秒，
会消耗 CPU/wall 预算。fs-verity 在此 native ext4 guest 可无特权启用，而
WSL ext4 返回 ENOTSUP，因此仅可作为后续优化，不能取代当前通用安全路径。
本批 guest 验证覆盖 QtCore 后端；应用完整编译、全量测试及源码门禁由主线批次执行。

2026-11-14 最终独立验证：

| 验证项 | WSL | 非 WSL Linux guest |
|---|---|---|
| 快照/协议/controller QtTest | 13 通过，0 失败，0 跳过 | 13 通过，0 失败，0 跳过 |
| 固定运行时完整强隔离矩阵 | 17/17 | 17/17 |
| worker exec 后宿主退出 | 所有后代回收 | 所有后代回收 |
| cgroup memory.max / swap / pids / CPU quota | 512 MiB / 0 / 8 / 单核 | 512 MiB / 0 / 8 / 单核 |

矩阵还断言首次 stdout/stderr 溢出保留恰好 cap 大小前缀、非零退出 stdout
诊断恰好 logBytes、1 MiB stdout 与 4 MiB 输入并发完整传输、32 MiB 大型
输入被总体宿主预算拒绝，以及准备阶段过期时 CPU 统计为零（没有启动 worker）。
OS 峰值在 OOM 用例达到 536,870,912 字节；普通 NumPy 用例约 284–285 MB。
默认小任务宿主保留预算约 487 MB，均独立于子进程峰值。

`tests/tst_pythonsandbox_linux_native.cpp` 是显式 fixture 驱动的可选验收目标：
传入运行时根和宿主预期 manifest SHA，不接受外部 clone 路径。它在自有临时
目录中完整有界复制并再次校验 runtime，只有该副本可被篡改。缺失 fixture
直接失败；未配置供应的普通构建不注册这项执行。常规 13 项 QtTest 不需供应。
