# B｜脚本执行与私有资源生命周期（2026-11-20）

任务 B：#960（Windows 用户脚本取消/超时未回收进程树）与 #959（Windows
受控 Python 永久改写运行时与输入包 DACL）。工作分支
`codex/paleo-b-runtime-lifecycle-20261120`，独立 worktree
`../paleo-b-runtime-lifecycle-20261120`。

## 开工核对（scope-ledger）

- 基线：prompt 快照 `master@3e9df32c`（#1055）；实际开工 `origin/master@1a79b745`
  （2026-11-20 fetch）。与快照差异：#1051（P2 地震缓存）、#1054（P3 因子发布）
  已合入主干；开放 PR 仅剩 #1053（P4 ONNX，本任务不触碰其文件面）。
  #1049/#1050/#1052/#1055 均已合入，直接复用（deb 闭包前缀、ORT 搬移、
  bootstrap 入口、P1 导入完整性未重复实现）。
- 开放 issue：#960/#959 为本任务核心，均 OPEN（修复不以关闭 issue 为目标，
  Windows 真实验证未完成前不宣称解决）。主 checkout 保留用户未提交改动
  （paleomainwindow_attach.cpp 等），本任务全程在独立 worktree 内作业。
- 平台事实：开发机为 Linux（CachyOS）。Windows 原生验证（Job/DACL/LPAC
  用例）在本机无法真实执行——按任务边界如实报告为未验证项；Linux 侧做
  全量回归与源码闸门。Windows 用例代码随 PR 交付，待 Windows 环境执行。

## 工作包账本

| 包 | 状态 | 生产入口与改动 |
|----|------|---------------|
| B-W1 创建时进程树所有权 | 已实现 | 新增 `src/services/scriptprocessscope.{h,cpp}`：构造时建 KILL_ON_JOB_CLOSE Job（不设进程/内存/CPU 上限、不启用 breakaway）；`configure()` 经 QProcess 修饰器挂创建时 `PROC_THREAD_ATTRIBUTE_JOB_LIST` + `HANDLE_LIST`（仅三条标准管道可继承）；任一步失败→毒化启动结构→FailedToStart 拒绝，不回退无 Job 进程。测试接缝 `scriptprocessscope_test::forceCreateFailure`（链接期符号，生产零引用） |
| B-W2 ScriptRunner 退出收敛 | 已实现 | `Run` 独占 `scope`（launch 时创建，排队项不占资源）；cancel/timeout 递进档加 `terminateTree()`；finishRun 在发 runFinished 前完成整树收敛（Unix 补 `kill(-pgid,SIGKILL)` 收残留后代——组 id 成员存活期间不可复用；Windows TerminateJobObject+有界等待）；析构整树终止+有界等待，不依赖事件循环 |
| B-W3 REPL 闭环 | 已实现 | 会话代次 `m_generation` 隔离新旧会话（started/output/errorOccurred/finished/timer 全部按代次守卫）；三段停止态机（Polite→Terminating→Killing），timer 停止后立即销毁（修复 deleteLater 挂起窗口）；teardown 先整树收敛再有界接收残余输出、后发 finished；停止后重启中文往返有验收 |
| B-W4 认证私有视图 | 已实现 | `pythonsandbox_windows.cpp` 删除原 runtime/input 树的 `restrictTree` 逐节点 DACL 写（#959 根因）；新增 `pythonsandbox_windows_view.cpp`：runtime 按 manifestSha 内容寻址的私有视图（staging 流式复制+边复制边按 manifest 摘要校验→ready marker→原子改名），私有输入按 moniker 隔离；launch 的 program/codePath/workingDirectory/TEMP/TMP/LOCALAPPDATA 全部指向私有路径 |
| B-W5 权限/预算/租约 | 已实现 | 视图根一次性 PROTECTED+capability 只读可继承 ACL（子项创建时继承，不再逐节点 SetNamedSecurityInfo）；输入根按 run 授包 SID 只读；租约记录 `view-leases/<sha>.<pid>-<created>.json`（owner=PID+进程创建时间）；复用前完整重哈希（同尺寸篡改可检出）；磁盘余量前置检查；视图缓存 ≤2 代（marker 时间戳逐出，有租约不动）；单宿主 QSemaphore(1) 合同不变 |
| B-W6 journal 扩展恢复 | 已实现 | journal.cpp 的 `journalRoot/processCreation/ownerStopped/validMoniker` 提升为共享原语（v1 profile 记录读取约束零改动）；新增租约/staging/私有输入的死宿主恢复与 fail-closed 语义（记录损坏→保留+报错，归属未知→保留+报错；删除路径一律按根内重建，不采纳记录内绝对路径）；历史已改 ACL 的原树不做无依据“恢复”（无原快照，不猜测） |
| B-W7 生产入口 | 已实现 | provider probe→run 链路不变（dispatch→runWindowsSandbox→私有视图）；`SandboxResult` 增 `viewBytes/viewFiles/viewReused` 诊断字段并进 readiness evidence；视图阶段失败以具体码直报（python_view_*），清理失败保留既有 `python_sandbox_cleanup_failed`（输出移 outputLog、不进成功链）；PythonConsoleController 面零改动（终态语义由 runner 合同保持） |
| B-W8 故障证据 | Linux 全跑 / Windows 用例交付 | 注入接缝 `view_test::setFailuresForTest`（复制/ACL/租约/清理四档，仅测试置位）；Unix 真实进程树夹具 `spawn_tree.py`（父/子/孙+原子哨兵+管道继承）；Windows 用例（SD oracle/视图复用/篡改/注入/双宿主重叠）已写入 tst_python_windows_sandbox，待 Windows 真机执行 |

## 验证记录（Linux，deb 闭包前缀）

环境：`QGIS_PREFIX=/home/kevin/projects/paleo-deb-prefix-20261119/usr`（#1046
闭包，host 无 multiarch 故显式 `-DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`）；
测试直跑注入 `LD_LIBRARY_PATH/QT_PLUGIN_PATH/PROJ_DATA/GDAL_DATA`（同
paleo-p6 worktree 的 run-target-tests.sh 既有配方）。日期 2026-11-20。

| 命令 | 结果 |
|------|------|
| `cmake --build build --target tst_scriptrunner tst_pythonrepl tst_pythonconsole tst_scriptprocessscope tst_python_windows_unavailable -j4` | 全部编译链接通过 |
| `./build/tst_scriptprocessscope` | 3 passed, 2 skipped（Windows-only 用例按设计跳过） |
| `./build/tst_scriptrunner` | **20 passed, 0 failed**（含 4 个新进程树回收用例：主进程先退/取消/超时/析构，孤儿由 init 异步收尸故 oracle 为有界轮询） |
| `./build/tst_pythonrepl` | **10 passed, 0 failed**（新增 start 后立即 stop、停止后重启中文往返、后代连坐×2） |
| `./build/tst_pythonconsole` | **12 passed, 0 failed**（新增终态互异、REPL 重复启停） |
| `./build/tst_python_windows_unavailable` | passed（Linux stub：不授予执行） |
| 邻接回归：`tst_pythonprocess 3/0`、`tst_pythoninputbundle 6/0`、`tst_pythonruntime 30/0`、`tst_pythonsandbox_linux 13/0`、`tst_pythonenv 10/0`、`tst_pythondelivery 23/0` | 全绿 |
| 消费者回归：`tst_pythonanalysiswiring 12/0`、`tst_pythongeneratedoperations 9/0`、`tst_pythonsandboxreadiness 8/0`、`tst_pythonseismictracewiring 7/0` | 全绿（SandboxResult 加字段、runner/repl 行为收敛不破坏既有合同） |
| 源码闸门 `ctest -R "^(layering.*\|qgis_spread.*\|ui_invariants.*\|ui_size.*\|file_budget.*\|i18n.*\|ui_tokens.*\|qgis_versions.*\|docs_drift.*\|cpp_mainline.*\|ci_known_reds\|well_facies_input_audit)$"` | **30/30 通过** |
| `layering_transitive`（注入 QGIS_PREFIX） | 通过 |

## 已知边界与未验证项

- **Windows 真机验证未执行**（本机 Linux）：创建时 Job 归属竞态、逐节点
  安全描述符 oracle（nodeSecurityText/treeSnapshot）、LPAC 两宿主重叠
  （--view-overlap）、视图复制/ACL/崩溃恢复、`tst_scriptprocessscope` 的
  Windows 分支——代码已交付、未真机运行，不得据本页宣称 #959/#960 已解决。
- Windows 代码经不起本地编译验证（Q_OS_WIN 分支本机不编译）：以受控链
  既有模式（piprocessplatform_p.h 的修饰器毒化、journal 的 owner 判活、
  launcher 的 JOB_LIST/HANDLE_LIST）为蓝本编写，风险如实登记。
- Unix 语义不变项：setsid 进程组、递进停止窗口（2s/1s/1s）、UTF-8 管道、
  半行 1 MiB 上限、队列闸 ≤2、runStarted/runFinished 顺序合同——由 20/10/12
  个既有+新用例回归。
- 后台脚本生命周期边界（本批新语义）：运行结束即回收后台后代，Unix 依
  kill(-pgid)（组 id 复用受内核约束）、Windows 依 Job；主动脱离会话
  （start_new_session/breakaway）不在强回收承诺内。
