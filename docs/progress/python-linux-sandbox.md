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
不能借用未使用的 codeBytes；frame 的 4 字节长度头也计入完整传输 inputBytes。
输入/代码/manifest 与 output/log 的 broker
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
systemd 属性刚创建时可短暂显示无界值；barrier 在原有 5 秒准备期限内保持关闭，
只有四项实际限额全部符合才释放，持续缺项或错误值仍失败关闭。
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

固定运行时 CPython 3.14.10、NumPy 2.5.4，共 2,143 个普通文件、249,075,081 字节；
manifest SHA 为 `c7315dae25f2b4588e39fbba1f9d9037aa41de7a61c46909d05205d79929c821`。
Linux 生产文件继承 PY1 的公共 runner，没有复制进程泵或另建调度器。

### 原生完整应用矩阵

2026-11-15 使用独立 QEMU/KVM Arch guest（内核 7.2.11-arch1-1、2 vCPU、4 GiB）。
测试宿主的 4 GiB 为完整 QGIS/WebEngine 应用留出验证空间，单次 Python cgroup 仍为
原策略 512 MiB、swap 0、pids 8、CPUQuota 100%，没有放宽 CPU/wall 或测试断言。
普通 tester UID 1000 在私有 mount namespace 内运行，bounding/inh/ambient capabilities
均为零；系统 helper 保持真正 root 所有权，16 个文件 SHA 匹配既有 native profile。

pristine 镜像 SHA `794eb4889b39e1f0ed7d5ae998b2a1ed2137db8cf8c3476cf2962c7437ecb003`，
仅作只读 backing；独立 snapshot、固件变量、端口和经 serial/20 GiB/空白检查的新数据盘
均位于专用验证目录。不复用另一台在写的 guest root/data，不修改宿主包、设备 ACL、
组成员或全机策略。KVM 启动进程保持 UID/GID 1000，只临时补实际设备 GID，全部 cap 为零。
guest 仅从已验证缓存离线安装签名 bubblewrap 0.13.1，既有 systemd 262 无升级。

完整 `paleo` 和 44 个 C++ 测试目标在 WSL 分别编译 default/no-ORT，再把同一批 ELF
连同锁定 QGIS 4.2.3、GDAL 3.12.2、Qt/WebEngine 6.11.2 闭包送入 native guest。
编译各 lane 使用 `-j1`、`CMAKE_AUTOGEN_PARALLEL=1`，独立构建目录与产物。
完整应用验证没有用 QtCore 薄壳代替。原依赖闭包 13,934 个文件与两个 lane 的 6,449 个
文件分别在宿主和 guest 校验 SHA；每轮前后运行时 2,143 文件均精确匹配且保持只读。

| 验证 | default | no-ORT |
|---|---|---|
| 完整 `paleo` 与 44 个 C++ 测试目标编译 | 通过 | 通过 |
| CTest（44 C++ + runtime/scripts 供货 4 项） | 首轮 47/48；完整失败套件独占重试通过（见下） | 48/48，408.75 秒 |
| QtTest | 组合 632 通过、0 失败、0 跳过（非单轮） | 626 通过、0 失败、0 跳过 |
| 正式强隔离 fixture 内部矩阵 | 19/19，47.89 秒 | 19/19，56.78 秒 |
| app/QtCore/QtGui/WebEngine/QGIS/GDAL 六组 `ldd -r` | 通过 | 通过 |
| WebEngineWidgets 实际链接、Office 组件、应用首帧 | 通过（独立补验） | 通过 |

Chromium sandbox-disable 环境变量未设置；首帧 trace 同时含 `first_paint` 和
`startup_screen_closed`。私有 OS 兼容 overlay 复用 PY1 的 native Expat 2.9.0 文件
（SHA `0755d0589724746da578d47ee23bfa09ecee64d9bf8e6eab0c277e6757895395`），
没有覆盖系统或锁定 QGIS/Qt 闭包；旧用户 Python 控制台/venv 回归也真实运行。
首轮 driver 在进入 CTest 前因拷贝测试工具时丢失 execute 位返回 126；只恢复私有
`controller_probe` 的原 0755，内容 SHA 不变，保留该日志后完整重跑，没有修改产品代码。

正式矩阵覆盖 NumPy、二进制 frame、4 MiB 输入与 1 MiB stdout 并发完整传输、宿主文件/
环境/网络/SysV IPC/FD 隔离、CPU/OOM/wall、取消、输出和日志超限的精确前缀，
以及密封后同长度改写代码和 NumPy 原文件。4,096 字节完整 frame 可执行，4,097 字节
在执行前拒绝，CPU 为零；准备阶段到期同样没有启动 worker。OOM 实测峰值为
536,870,912 字节，普通 NumPy 观测峰值约 283–284 MB，宿主保留预算约 487.1 MB。
完整性与资源数据区分 host reservation 和 child peak，`memoryPeakComplete=false`。

另在 Python worker 真正 exec 后 SIGKILL 宿主，三个后代全部回收，存活与待 init 回收
集合均为空；实际限额为 memory 536870912 / swap 0 / pids 8 / cpu 100000 100000。
早期共享 2 GiB guest 的冷快照负载曾使观察器在 45 秒内没有看到 worker，该轮不算通过；
此次独立 guest 保持原观察期限与生产策略获得真实回收证据。

### 默认配置首轮 CPU 分类与重试

默认配置首轮 CTest 302.17 秒，47 套件通过，`tst_pythonprocess` 的 CPU 子例
返回 `python_process_failed` 而非预期的 `python_process_cpu_limit`；其余 17 行通过。
首轮没有捕获退出信号或最后 CPU 值，不能事后把它确定归因为 CPU/OOM。
同一原始 ELF、同一 CPU 1 秒和内存 96 MiB / wall 15 秒策略，虚拟机独占完整重跑
18 行全部通过，8.291 秒。外部 1 ms watcher 观测 CPU scope 累计 1,012,141 µs、
OOM 事件为零，不能用重试的计数倒推首轮原因。公共 runner 已说明 cgroup FD 不保证
收集后的统计继续可读；极短任务的末尾统计和退出原因仍可能不完整。

最终组合为首轮其余 47 套件与完整失败套件重试，QtTest 632/0/0；首轮实际为
631/1/0，没有改写成单轮 48/48。默认 app 首帧单独补验通过。原生产代码、资源限额、
超时、容差及断言均保持，失败输出仍拒绝作为成果发布。

### WSL 与可选测试注册

当前 WSL Microsoft 6.6.87.2 的 user manager 仅委派 memory/pids，缺少 CPU。
同一最终后端真实返回 `python_linux_resource_limits_unenforced`，CPU 0、stdout 空；
没有关闭 required fixture、伪造 CPU 配额或改 WSL 全机策略取得绿灯。WSL 完整编译与
13/13 快照/协议/controller 单测通过，不能据此宣称当前 WSL 可执行强隔离任务。
早期不同委派状态的 17 项探针不计入本轮原生完整矩阵。

`tests/tst_pythonsandbox_linux_native.cpp` 通过 `add_paleo_test` 使用最小 QtCore/Test
链接集注册。默认关闭；显式启用时要求 Linux、Qt >= 6.7、现存 fixture 根与宿主 SHA：

```sh
cmake -S . -B build -DPALEO_TEST_PYTHON_LINUX_NATIVE=ON \
  -DPALEO_TEST_PYTHON_LINUX_NATIVE_ROOT="$VERIFIED_RUNTIME_ROOT" \
  -DPALEO_TEST_PYTHON_LINUX_NATIVE_SHA256="$HOST_MANIFEST_SHA256"
ctest --test-dir build -R '^tst_pythonsandbox_linux(_native)?$' --output-on-failure -j1
```

开启配置的 fixture 缺项直接失败，零跳过；未供应的普通构建不注册这项执行。
测试只在自有临时目录完整有界复制并再次校验 runtime，只有该私有副本可被篡改，
不接受外部 clone 路径。Qt < 6.7 / 非 Linux 选择明确不可用 stub；当前 Qt 上强制选择
stub 的 3/3 测试通过，不把这项当作实际 Qt 6.5 编译证据。

每任务重新完整 SHA、memfd 复制和逐文件挂载有明显成本：本轮 cold NumPy 约 10 秒，
后续任务约 2–6 秒，均消耗实际 CPU/wall。早期资源争用下同任务达到 62 秒，
不能把最佳时延当保证。fs-verity 在 native ext4 可无特权启用，WSL 返回 ENOTSUP；
它仅是后续优化方向，当前没有按路径/mtime 命中的校验缓存。

最终基底 `ff2b9b297342e8df5e166747f86afc709d1b6a66`，先 fetch/rebase `origin/master` 后源码门禁 37/37；
QGIS_PREFIX 使用权威锁定头，含全部 transitive 指标，基线没有扩大。
原始证据在本 worktree `local/py3l-native/` 和 `local/review-20261114/source-gates/`：
逐 lane payload/运行时 SHA、身份、六组 ABI、CTest/QTest、matrix、首帧、hostdeath、
WSL 拒绝与首轮工具权限失败日志均保留。
