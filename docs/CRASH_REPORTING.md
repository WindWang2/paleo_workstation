# 崩溃报告机制（本地转储）

> wave4/runtime-resilience。关闭 `docs/PALEO_QGIS_PLAN.md` §38 的留白决策
> （「本地转储 or 回传——留作实现期决定」）与 `TODOS.md` P3 autoplan pass-2
> 的「运行时韧性」递延项。

## 决策记录：本地优先

- **本地转储，不联网、不回传。** 崩溃时把报告写到本机
  `QStandardPaths::AppDataLocation`（Linux:
  `~/.local/share/paleo_workbench/crash/`，应用名在 `main()` 启动最早期钉住，
  不随 QgsApplication 的改名漂移）。不引入 breakpad/crashpad 等第三方依赖。
- **隐私边界。** 报告只含：崩溃时间（UTC 秒）、信号、应用/Qt/QGIS 版本、
  当前工程路径、pid、调用栈符号（`backtrace_symbols_fd` 的原始输出——通常
  只有模块基址+偏移，不带源码与用户数据）。不收集工程内容、不收集文件名
  以外的任何用户路径以外的信息；报告永不离开用户机器。栈回溯的
  `backtrace()` 在 POSIX 上不是严格 async-signal-safe（glibc 事实上安全）——
  这是行业通行妥协，接受并在此记录。
- **落点为何不在工程目录。** 脏退出检测只可能发生在「下次启动、任何工程
  尚未打开」的时刻，落点必须先于工程存在；报告头里的 `project:` 行
  （工程打开后经 `CrashReport::setProjectContext` 更新）已携带工程位置，
  §38 的会话诊断包需要时可从该行取到。

## 文件格式

`<AppData>/crash/YYYYMMDD-HHMMSS.txt`（UTC；同一秒第二份起加 `-2`/`-3` 后缀）：

```
paleo crash report
time: 20260926-214503
signal: 11 (SIGSEGV)
app: paleo_workbench (built Sep 26 2026)
qt: 6.11.2
qgis: 4.2.2-Belém do Pará
project: /home/user/projects/some_area
pid: 12345
--- backtrace ---
./paleo(+0x4a3f2)[0x55d2...]
...
```

- 无 execinfo 的平台（Windows）最后一节写
  `（此平台无 execinfo 回溯——仅旗标+时间戳报告）`，其余行一致。
- `.running` 会话旗标同住该目录：`pid=<n>` + `started=<UTC 时间>`。

## 实现结构

| 层 | 文件 | 说明 |
|---|---|---|
| 组件 | `src/services/crashreport.{h,cpp}` | 公共头只含 `QString`（POSIX 头不进公共路径）。Qt 侧（启动期路径解析/旗标读写/报告头预渲染）与 fd 侧（崩溃处理器只碰 `open/write/close/backtrace`，时间戳与信号号手工排版——`snprintf/strftime` 非 async-signal-safe）分离。同秒冲突用 `O_CREAT\|O_EXCL` 循环加后缀。 |
| 接线 | `src/app/main.cpp` | `installCrashHandler()` 在任何 QGIS/Qt 对象之前执行；`recoveryNoticeText()` 非模态警告框（§38 错误呈现契约：可恢复降级不模态）；`exec()` 正常返回后才 `clearRunningFlag()`。 |
| 工程路径 | `src/app/appcontext.cpp` | `projectOpened` → `setProjectContext()`（只改报告头，不改落点）。 |
| 信号面 | POSIX `sigaction`（SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS，`SA_SIGINFO\|SA_RESETHAND`——写完报告 `raise` 回默认处置，保留 core 语义）；Windows CRT `signal()` 最佳努力 + `_exit`。 |

## 恢复流程

1. 崩溃：处理器写报告（尽力而为，无重试预算）→ 进程按原信号终止；旗标残留。
2. 下次启动：`installCrashHandler()` 看到残留旗标 → `previousDirtyExit=true`，
   并按文件名取最新报告路径。
3. 提示：非模态警告框——有报告：「上次未能正常退出 Paleo。崩溃报告已保存
   在：<路径>」；无报告（如 `kill -9`/断电）：「上次未能正常退出 Paleo
   （没有生成崩溃报告）。」干净退出不弹任何东西。
4. 排障者直接打开报告文件（纯文本）即可对时定位；无需任何上报操作。

## 测试与代码指针

- `tests/tst_crashreport.cpp`（`tst_crashreport`，offscreen）：
  - 未装处理器时写报告安全拒绝（`writeWithoutInstallIsRejected`）；
  - 旗标生命周期与脏退出检测（`flagLifecycleStartClean` /
    `dirtyExitDetectedWhenFlagSurvives`）；
  - 报告生成/文件名/格式（`reportGeneratedWithExpectedFormat`：
  time/signal/app/qt/qgis/project/pid/backtrace 各行与
  `YYYYMMDD-HHMMSS.txt` 命名）；
  - 工程上下文随 `setProjectContext` 更新（`reportRespectsProjectContext`）；
  - 同秒冲突双落盘（`reportSameSecondCollision`）；
  - 最新报告选取（`latestReportPicksNewest`）；
  - 重启提示文案三态（`recoveryNoticeTextCoversStates`）。
- 信号处理器本身不进单测（fork 触发信号的用例不稳定）；其落盘核心与
  `writeReportForSignal()` 是同一函数（`crashreport.cpp` 的 `writeReportFd`），
  格式测试即覆盖处理器输出。
