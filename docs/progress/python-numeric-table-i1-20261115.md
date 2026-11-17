# I1：声明式数值表输入与独立报告链

日期：2026-11-17。当前代码 HEAD `f1bdd10538d45139b850b637453de98d053d36fe`，SDK `f329d93d4ed4b9c60fbc6c92b49ea5953950daf7`。源码版本、实跑版本及精确范围分列。

## 范围与用户链路

用户先明确声明 SDK 数值投影，再用 SourceRef、table ID 和有界窗口查询元数据；Host 解析实际 Catalog 文件父版本及完整 SHA，冻结独立 typed InputBundle。1–8192 行、最多 15 个值列采用 Float64／Boolean／显式 null mask，另有保持 int64 的物理行轴；不接受模型路径、SQL、猜测类型／单位或自报 Catalog 版本，也不自动声明投影。

真实执行遵循独立 prepare → compute-only → 私有 REPORT 认证分页 → 另行授权 publish／新 Host 历史身份恢复。generated-only 权限与真实强后端缺一不可；缺 fixture 或后端必须失败，不 skip、不回退普通解释器。SDK read cut 是实际观察的 SQLite 读切面，不是历史数组租约。旧 Generated v1 保持自己的 ID、validator 和 restorer，CSV／PNG 属后续批次。契约依据为 `docs/PYTHON_NUMERIC_TABLE_INPUT.md`。

本次对齐相关声明名并注册 `python.numeric_table_compute` 命令元数据；inventory 和 phase-effect 回归保持 prepare／compute 不产生正式版本。继承 A2 的工具结果视图夹具与 E2 Host 准入修复，不从命令目录复制参数默认值或授权，也不扩大 SDK 取数范围。当前 Linux 已覆盖编译／普通验证，新的真实业务链留给当前 O2 联合验收。

## 代码身份与分平台接续

本次 restack 仅增加 `Q_OS_WIN` 条件内的六行测试清理，产品代码、全部原断言及 Linux 适用业务代码未变；删除该精确插入块可还原前一 Git blob。当前代码 HEAD 不等于下列每份旧实跑收据的 HEAD；不宣称 Linux 全量重跑、Native 重跑或 ELF 调试行号元数据相同。

- 原完整 Linux 构建／ordinary：`fb1cca096b8d5639d19695f0e32eae01a613f44c`，保持原 b047／6096 入口及下表范围。
- 后续 alias 单目标增量及已执行 Native 的依据 HEAD：`83d9524113aae7e7d4c8139eabff9736630b5358`；其 source/raw/SDK 和失败历史不改标为当前 Windows-only HEAD。
- Linux ort alias 增量：`tst_pythonanalysiswiring` actual 0，raw `d31111da52b20f78f80d9164d245801eadffc3dd8282ace6a4786b3089c1da4e`；ordinary／GUI 未重跑，其余 ELF 按原收据的 stat unchanged 保留。
- Linux noort alias 增量：`tst_pythonanalysiswiring` actual 0，raw `d31111da52b20f78f80d9164d245801eadffc3dd8282ace6a4786b3089c1da4e`；ordinary／GUI 未重跑，其余 ELF 按原收据的 stat unchanged 保留。

本次测试 TU 的 Git LF SHA `2a4d02416cee602a553e0fce29b036d4ddd8cebdb670ab4ba5b759be7490d35c` 与 Windows live CRLF SHA `d3c19285c6427b9c65713d71dd3e55b6d50aa0f707a1074aa0d25d3cfe742ead` 分列；whole-tree raw 只取实际构建／运行收据，计划派生值不当作实跑身份。

原完整构建／ordinary（保持当时 identity）：

| 原平台与配置 | 原已证结果 | 原边界 |
|---|---|---|
| Linux／WSL 默认 | 完整 **73** 目标构建退出 0；10 个 ordinary 套件，Qt **156/0/0**；Office／WebEngine／first_paint／startup_screen_closed 通过 | WSL 精确拒绝 **3/0/0**，不是强执行正例 |
| Linux／WSL no-ORT | 完整 **72** 目标构建退出 0；9 个 ordinary 套件，Qt **142/0/0**；同范围 GUI／组件检查通过 | WSL 精确拒绝 **3/0/0**，不是强执行正例 |

## Windows 实际验收与分域范围

- Windows ort：当前 HEAD 完整 72 目标构建及最终运行身份核验完成。
  当前 final raw `c687fca313d30a9aa0332bddb82c4b973655f5a0cb885782bacf96909627ed05`；ordinary 22 次 Qt 271/0/2/0，实际 exits [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]，allGreen=True；强执行 4 次 Qt 13/0/0/0，首帧 exit 0。各阶段原 HEAD 保存在索引，不统一改标。
- Windows noort：当前 HEAD 完整 71 目标构建及最终运行身份核验完成。
  当前 final raw `c687fca313d30a9aa0332bddb82c4b973655f5a0cb885782bacf96909627ed05`；ordinary 21 次 Qt 251/0/2/0，实际 exits [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]，allGreen=True；强执行 4 次 Qt 13/0/0/0，首帧 exit 0。各阶段原 HEAD 保存在索引，不统一改标。

本轮 E3 至 O2 Windows Runtime 分域只把 `changedFixtureIsReauthenticatedBeforeStrongExecution` 留在既有 strong Native 域；ordinary 保持其余 17 个方法原顺序，包括 `invalidManifests` 的八条数据行。两域为原 18 方法的精确不交并集，原 strong 同函、断言、fixture、REQUIRE、预算均不减。分区本身不计运行结果，实跑状态仅取本轮对应收据。

Windows Qt 四项依次为 passed／failed／skipped／blacklisted；其他原三项按原口径。次数含各次 init/cleanup；不把 direct QtTest 记成 CTest 全绿，也不合并原失败与复跑以伪造首轮 0。

## 已执行 Native：保留真实版本与范围

本批自身 Native 没有本轮重新跑完整矩阵；只引用其原 head/raw/Pi tuple，以及 O2 联合验收的真实 scope，不声称逐批新 HEAD 全量 Native。

统一索引中的五路完整 Native 共 140 项、Qt 1539/0/0，另列上述默认显式 composite；所有版本、scope 与首次失败均保留。Windows-only 条件块不改变 Linux 业务有效代码，因此没有再次启动机器验证或产品运行。

## 更早历史 Native：保留原身份

以下仅为旧十二路交接 `652151751c79022b6764faa830290a81b7579f0ed2946eb389aedaea882b6ff4` 的本批两路，**不是文首新 HEAD 的 Native 结果**。原 head `349bc0bbbf7d67b2c71770dfe2fa6a3deba3442a`，SDK `f329d93d4ed4b9c60fbc6c92b49ea5953950daf7`，raw `f00fd4e5d08f1fd60ea73ab43442899ada77c8588607896a25194ee8b95d7ec8`；旧汇总明确 `newDispatcherSurfacePatchNotCovered=true`。

| 原配置 | 实际执行范围 | Qt passed/failed/skipped | run 秒 |
|---|---|---|---:|
| 默认 | 5 C++ + 8 source gates＝13 项 | 51/0/0 | 86.883 |
| no-ORT | 5 C++ + 8 source gates＝13 项 | 51/0/0 | 55.353 |

上述两份本批 summary 各自核定以下 Pi tuple，一致性来自本批原件核对，**没有统一套用 O2 身份**：

- root：`/home/kevin/paleo-pi-a2-store/generations/ce74ce4fe0c5e26a07f673c65a4a3c27a559c18f806f3e2b6f32fbb58aaf4374`。
- generation／manifest SHA：`ce74ce4fe0c5e26a07f673c65a4a3c27a559c18f806f3e2b6f32fbb58aaf4374`。
- sourceLock SHA：`966289527db156efa006db7d93e00d1c9e1a482473e36a45f1942949faaec79c`。
- 每路的 9 个 Pi 文件及 runtime 均按原 before／after／final 原件保持集合、SHA 与只读；六项 ABI、首帧按旧记录通过。当前没有重读这些正文或重跑该旧矩阵。

旧默认 summary：`/mnt/c/Users/wangj.KEVIN/projects/paleo-python-delivery-e3-20261115/local/stack-validation/native-window-16rounds-20261116/related-05-i1-ort/summary.json`，SHA `9d6529fa6e81a126da00bb2c92adfb6c503483d6baa11e7dd4f2c601147b82ca`；旧 no-ORT summary：`/mnt/c/Users/wangj.KEVIN/projects/paleo-python-delivery-e3-20261115/local/stack-validation/native-window-16rounds-20261116/related-06-i1-noort/summary.json`，SHA `e2f2d6b9e3d37c2b125e4bb42eb152264c350f666b73806e7d4c69ee91c1c303`。制包／传输／运行／收集和首次失败按各自原索引保留，不把重试或新机器身份写回这些记录。

## 边界与证据

外部分发入口仍关闭；Shell、任意 CLI／SQL／HTTP、在线安装包与持久解释器仍后置。prepare／compute 只产生私有成果，publish 独立；生成数值与像素未经宿主独立重算。Pi 是可信可选编排组件，不是 OS 隔离沙箱；缺强后端不 fallback。Office／WebEngine 与 offscreen 首帧不扩写为 Office WASM DOM 验收。

证据索引位于操作者本地 E3 worktree 的 ignored 路径 `local/final-execution-20261116/final-docs-20261117-r2/reconciled-progress/evidence-index.json`，未入仓库，也不是本进度文档所在目录的仓库文件；对外审阅需另附实际可访问的证据附件或链接。该本地索引分列原完整构建／ordinary、alias 增量、各 Native 原身份与 composite、新 Windows-only proof、有限 O2 Linux crosscheck、动态 revision4 completed/final、原失败及四份 reviewed 全局文档 manifest。原 writer、原输入及生产文档／源码／refs 不修改。

本批分支 `codex/python-numeric-table-input-20261115`，直接父分支 `codex/pi-agent-a2-20261115`；共同 master 基底为 `e995a477`。跨仓前置为 [Data_service #22](https://github.com/changyanyanchang/Data_service/pull/22)，SDK `f329d93d`。 实际审阅 HEAD、直接父 PR 及 fetch/rebase 后的 Source gates 结果以本批 PR 正文为准；运行证据继续保留本文列出的实际代码身份，文档提交不重标原实跑版本。


## Windows 队列终态收口范围

E2 至 O2 的十六个 profile 已有真实 build／final 与独立 audit 收据；E2 默认仍按旧全量加新单 TU 的组合等原范围记录，不把原失败或旧实跑版本改标。队列终态以新核对命令实际 exit 0 的 `terminal_reconciliation` 纯元数据收口为准；原工具 session 87628 的 outer exit 因 session 失效仍为未知，不能写成原 outer 0。18823／81832 的原 exit 1 永久保留；本次收口未重新运行 C++、测试或依赖大树 SRI。owner 原件、精确 scope、各旧退出事实与 SHA 均链入上述操作者本地 ignored 证据索引。
