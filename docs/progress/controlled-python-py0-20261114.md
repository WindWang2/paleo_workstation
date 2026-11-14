# 受控 Python PY0：可选环境与执行契约

日期：2026-11-14。先按用户要求合并 #943、#944，主 checkout 同步至
master `17bdfd0f2a4f81ca37732795cbb7e3166816e6be`。合并后核对分支保护恢复，
原未跟踪文件与六项 stash 保留。PY0 在独立 `codex/python-runtime-contract-20261114`
worktree/build 实施，SDK 为 `8c0a3295947f8dad38f07c6d1226678b16d88aa3`。
本批只交付环境身份、预算与只读状态，没有可信脚本或生成代码执行工具。

## 实现

`paleo_services_pythonruntime` 是仅依赖 QtCore 的 C++ 薄库。宿主明确提供 generation
根与清单 SHA；逐文件验证固定源锁、平台、版本、尺寸、模式及完整 SHA，拒绝额外文件、
链接、大小写别名与篡改。清单及文件读取有独立字节上限，取消不会发布部分就绪状态。
固定 CPython/NumPy 探测以清理后的环境运行，不从宿主 PATH 发现解释器。

Provider 的快照不等待正在进行的验证；等待 inspect 的调用可取消。探测通过现有
PaleoTaskService 调度；taskAdded 同步删除订阅者也不会交付迟到结果。状态工具
`paleo.python_environment` 只读且无参数，拒绝代码、路径或授权覆盖，缺环境时返回具体
原因。LocalExecute 保留独立效果位，所有执行开关默认关闭，本批不授予执行权限。

`analysis-core.v1` 上限为代码 256 KiB、输入 256 MiB、输出 64 MiB、每个日志通道
1 MiB、工作区/子进程内存各 512 MiB、CPU 60 秒、墙钟 120 秒、线程及并发各 1。
宿主只能收紧数字。它们是后续执行后端的契约，本批没有宣称 OS 沙箱已交付。

可选供货锁定 CPython 3.14.10 与 NumPy 2.5.4，源锁 SHA 为
`109899e2011cde44e1ed7cab28a962af7e0273ee9eaab38cec34c44da8554014`。
Windows 使用 PSF embed，Linux 使用 Astral python-build-standalone 20261110；
来源、许可、ABI 与精确变换见 [供货说明](../../vendor/agent-python/README.md)。
开发/运维离线准备工具校验归档并原子切换 generation；下载仅显式 `--fetch`，Agent
无法调用准备工具。升级不修改或删除旧 generation，使用 lease 与持久 operation 引用
是未来 GC 的共同约束，本批不提供运行时 GC。

默认安装不带该可选 Python 包，`src/` 无 Python 源码；没有 PyQGIS、pip 执行或内嵌
解释器。既有人工控制台/REPL 的发现规则保持独立。

## 验证

两个独立完整应用 build，锁定 QGIS 4.2.3 / GDAL 3.12.2 / Qt 6.11.2，
WSL Arch Linux / GCC 16.2.1 / CMake 4.4.3。默认 ORT 为 1.30.0，另建无 ORT 配置。
共享 Office 编辑组件已就绪，编译总并行不超过 8，各 build 的 CTest 串行。

| 配置 | CTest | QtTest 通过/失败/跳过 | 最终 CTest 耗时 |
|---|---|---|---|
| 默认 ORT | 27/27 | 360 / 0 / 0 | 165.65 s |
| 无 ORT | 27/27 | 354 / 0 / 0 | 164.08 s |

包含新 runtime/environment、原控制台/REPL、宿主/恢复/凭证、保留/清理、聊天与命令
相关回归。目录规模用例显式启用 `PALEO_CATALOG_SCALE=10000`，没有把跳过计为验收。
QtTest 总数含初始化、清理与数据行；供货检查及其 11 项自测另计入上述 CTest 套件。
首次完整运行发现旧工具表固定数量假设及定时并发夹具问题；修正后重新运行整组，
没有只重跑失败项来代替最终整组结果。

真实运行时逐文件验收：

| 平台 | manifest SHA | 文件/字节 |
|---|---|---|
| Windows x86-64 | `94446a0c28dc1f2b2b8541f8e0151e964aff5b7ab36f2868742fbd553dc665ad` | 982 / 66,435,629 |
| Linux x86-64 | `c7315dae25f2b4588e39fbba1f9d9037aa41de7a61c46909d05205d79929c821` | 2143 / 249,075,081 |

两平台均实际 import 固定 NumPy 并计算点积；离线供货自测各 11/11。
Linux 34 个 ELF 的闭包检查零缺失，外部 OS ABI 依赖与 glibc 下限已明确列出。
并行 build 的运行时夹具各自复制到 build 私有 generation，避免测试抢同一 lease。

Windows MSVC 19.38 / Qt 6.8.0 Core/Test 独立壳最终 18/18，零失败/跳过，
真实固定环境在该原生进程中验证。生产 CPP 的 Ninja 依赖 155/144、测试 CPP 305，
均 VALID 并含真实头；只链接 Core/Test 与 CRT，没有混入 QGIS。这是原生薄库验证，
本批没有执行 Windows 完整应用构建，也没有以它声称完成 Windows 隔离。

两套完整应用 `ldd` 无未解析依赖，ORT 链接状态与配置匹配。移除 Agent 环境配置，
在各自 XDG 沙箱中以 `PALEO_STARTUP_EXIT_AFTER_FRAME=1` 实际启动并在首帧后正常退出。
offscreen 的 OpenGL/窗口插件提示记录在日志中，不视为人工视觉验收。

最终 fetch/rebase `origin/master` 后，完整 Source gates **35/35 通过**。
完整命令与日志保留在 worktree 的 `local/python-validation/`，两个 WSL build 的
`py0-{final-build,final-tests,counts-report,closure,smoke}` 文件及 Windows
`local/python-validation/native/`。最终 Source gates 以 CI 注册清单及附加自检执行，
显式提供锁版 QGIS 头目录，结果保存在 `local/review-20261114/source-gates/`；
没有增加任何基线。后续 PY1–PY4 仍须分别完成相应验收并提 PR，Shell 后置。
