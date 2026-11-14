# 受控 Python PY1：可信模板与既有操作链路

日期：2026-11-14。本批在独立 `codex/python-script-operations-20261114`
worktree/build 实施，依赖 PY0 最终环境契约及供货锁。内部验收能力
`python.trusted_summary` 使用宿主编译时锁定的 `summary.py`，仅接受有限数值数组，
没有向 Agent 开放自由代码、脚本路径、启动参数或环境安装入口。业务分析工具在 PY2 接入。

## 实现

可信脚本 adapter 复用 CapabilityHost 的冻结、授权、幂等与恢复分派；进程在既有
PaleoTaskService worker 中运行。全进程只提供一个可取消的执行许可，输入准备和
worker 携带同一许可；等待许可不计入子进程执行墙钟，不另建任务调度器。

InputBundle 将 `code.py`、`request.json` 与来源元数据写入工程私有、内容寻址的
输入 generation，冻结脚本、运行时、输入及 policy 摘要。验证拒绝链接、路径别名、
额外文件、同尺寸篡改及代码/输入/暂存总预算超限；纯验证不创建缺失目录。运行时使用
引用保留现有 generation，持久操作保留输入快照；本批没有新增自动 GC。

子进程使用清理后的白名单环境和固定隔离启动选项。执行脚本读取、SHA 核验与传输
使用同一份内存字节，验证后替换磁盘脚本不能替换实际执行内容。stdin/stdout 同时
有界泵送，超时、取消、CPU/内存预算、退出失败和输出洪水均返回具体代码。失败的
stdout 只保留有界诊断前缀，不能成为数值结果；stderr 与执行状态也作为私有诊断端口。

Windows 以挂起创建、先加入 Job 再恢复的顺序约束进程树；Job 负责内存、累计 CPU、
活动进程及宿主退出时的回收。Linux 先建立 systemd 用户 scope 和 user/PID namespace，
核对实际 memory/swap/pids/CPU controller 限额与记账文件后才释放执行屏障；累计 CPU
按整个 cgroup 计量。辅助程序及依赖使用显式根所有权/SHA 锁，不从 PATH 发现或降级。
Linux Qt 低于 6.7 的构建保留可编译的 unsupported 实现，明确拒绝执行。

本批是可信模板进程底座。可信子进程仍具有用户文件系统/网络权限，未宣称文件、网络、
IPC 安全沙箱，也不允许生成代码复用此模式。Linux 内存峰值为存活期间实际观察到的
最大值，`memoryPeakComplete=false`；Windows 可保留完整 Job 峰值。数值库线程环境
和 CPU 带宽控制不等同于逐线程数量的安全边界。完整强隔离由 PY3 分平台验收。

计算只产生私有 REPORT 端口，宿主再验证严格 UTF-8/JSON、输出契约、有限数值和完整
SHA，沿用已有输出集与 Catalog 成组提交。LocalExecute 与发布权限独立核验并持久保存，
prepare/compute-only 不登记正式成果。恢复重建原脚本/输入/运行时身份；已提交操作不
重复计算或发布，显式取消不因重传或 computed 复用而返回成功。文档/epoch/授权变化、
订阅者销毁和同步提交回调重入均有终态检查。

## 验证

两个独立 Release 完整应用 build：WSL Arch Linux / GCC 16.2.1 / CMake 4.4.3，
固定 QGIS 4.2.3 / GDAL 3.12.2 / Qt 6.11.2，默认 ORT 1.30.0 与无 ORT 两条车道。
Ninja 各 `-j2`，后续 configure 显式 `CMAKE_AUTOGEN_PARALLEL=1`，避免 AutoMoc
自行增加子进程。最终进程底座修正后，两套均增量重编并重新链接完整应用及全部测试目标。

最终执行在独立 KVM Arch Linux 7.2.11 guest，采用上述相同 ELF，保留编译时的源/资源
路径映射；私有 mount namespace 内以普通 UID 1000、所有 capability 集为零运行。
辅助程序仍是真实 root 所有权，没有 user namespace 伪造所有者。31 个 C++ 套件
（含 OfficePreview / WebViewPanel）及两项离线运行时供货检查全部串行执行，显式要求
真实锁版 CPython/NumPy，目录规模夹具为 `PALEO_CATALOG_SCALE=10000`。

| 配置 | CTest | QtTest 通过/失败/跳过 | 最终 CTest 耗时 |
|---|---|---|---|
| 默认 ORT | 33/33 | 458 / 0 / 0 | 380.34 s |
| 无 ORT | 33/33 | 452 / 0 / 0 | 468.33 s |

每条车道从完整 SHA 核验的离线包新建只读私有 runtime generation。应用/测试/资源
传包的 19,515 个文件、4,333,453,029 字节在 guest 按宿主锚逐文件复核；manifest SHA
为 `6660339564b28cb642237fc1f34e48b4b1423b7964083f0584b2183e62298124`。
测试依赖的生产工程创建工具 `paleo_mkproject` 另核对两端相同完整 SHA 后接入。

原生验证环境使用私有 OS 兼容目录：guest 系统 Python 的 pyexpat 需要较新 Expat
符号，而 Qt 闭包原有版本会遮蔽它。将 guest 原生 root-owned
`/usr/lib/libexpat.so.1.13.0`（Expat 2.9.0）复制到独立 `native-os/lib` 优先目录，
SHA 为 `0755d0589724746da578d47ee23bfa09ecee64d9bf8e6eab0c277e6757895395`。
系统库和原固定 QGIS/Qt 包没有覆盖；Agent 运行时及后端辅助程序的清洁环境保持独立。
真实 venv、pip 模块导入正常后，对应用、QtCore、QtGui、WebEngine、QGIS 与 GDAL
六组分别运行 `ldd -r`，均无缺失或未定义符号。首轮环境失败日志保留，补齐环境后
重新运行完整两组，最终计数不包含首轮失败结果。

两套主程序均实际链接 Qt WebEngine Widgets，构建内 Office 编辑组件完整存在；
移除 Agent 环境配置，在各自 XDG 沙箱中以 `PALEO_STARTUP_EXIT_AFTER_FRAME=1`
启动并正常退出。启动环境显式移除禁止 Chromium 沙箱的开关。这是嵌入式链接及首帧
启动验收，没有声称人工视觉验收或重新执行 PY0 的 Office 文档 DOM 探针。

当前 WSL 的用户服务只有 memory/pids 委派，缺少 CPU controller，因此其完整测试
中进程/操作两项被如实拒绝，不能代替原生 Linux 的最终执行证据。针对 scope 属性
短暂过渡的回归始终保持执行屏障：同一最终底座探针在原生 Linux 严格限额下 376 ms
正常执行；在 WSL 持续缺 CPU 限额时 5,111 ms 后拒绝，输出为空且执行标记未产生。
部署仍要求匹配钉版 helper profile、用户 manager 及完整资源委派；缺项没有弱模式回退。

Windows 原生薄库使用 MSVC 14.38、Qt 6.8.0 Core/Test，进程 17/17、InputBundle 6/6，
均零失败/跳过。显式要求真实锁版 CPython/NumPy 执行，覆盖双向大流、脚本替换、
非零退出、输出/日志边界、CPU/内存、取消排队、跨线程许可及宿主死亡后的后代回收。
9 个对象的真实 Ninja 依赖均 VALID，当前 CPP 与证据源码 SHA 全部一致；动态依赖只有
Core/Test、Advapi32 和系统 CRT，没有 QGIS/Widgets。这是原生薄库验收，未执行 Windows
完整应用构建，也不以它代替 PY3 Windows 安全隔离证明。

最新基底上的完整 Source gates **35/35 通过**，显式提供锁版 QGIS 头目录，包含
transitive 四指标与 C++ 主线边界，没有增加基线。完整命令/原始日志保留在
`local/python-validation/`、`local/review-20261114/source-gates/`、两个独立 WSL build
及原生 guest 的 `p1-python-operations/{ort,noort}/build/py1-native-*`；
Windows 薄库证据在 `local/python-validation/thin/`。
