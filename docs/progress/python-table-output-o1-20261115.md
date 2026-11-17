# O1：REPORT 与宿主数值 CSV 成组成果

日期：2026-11-17。当前代码 HEAD `21e99639fbc4f62c37b6d3fe981a8acb9d53bcd3`，SDK `1362a69e61a663aba2b5024bfa83627af0759354`。源码版本、实跑版本及精确范围分列。

## 范围与用户链路

独立 `python.numeric_table_outputs` 冻结数值表输入与输出声明。用户先 prepare，compute-only 仅生成私有 REPORT＋TABLE 完整组；经认证分页查看报告和宿主 CSV 后，再用原操作引用与空参数 publish，原子登记整个组。幂等重试、历史身份和新 Host 恢复继续消费同一 durable operation／outputset／receipt，不重跑程序，旧 REPORT-only v1 不升级为本契约。

表声明限制 1–16 列、1–8192 行；首列是严格递增 int64 source_row 或显式有限物理轴，其余为有限 Float64／Boolean 与 null。固定列 ID 用作 CSV 表头，标签和单位只作宿主元数据，不解释自由文本、公式、隐式单位转换、排序、join 或重采样。REPORT≤256 KiB、TABLE≤2 MiB、CSV≤4 MiB，原始帧及组还受 12 MiB 和更小 policy 上限约束。

子进程只写有界 stdout frame v2，不交付文件路径或 MIME。全部端口、尺寸、类型、mask、实际 SHA 和 REPORT 来源均通过后宿主才编码 CSV；任一端口、取消、预算或后期写入失败不暴露部分业务组。契约依据为 `vendor/agent-python/NUMERIC_OUTPUT.md`。

本次注册 `python.numeric_table_outputs` 并对齐声明参数名；共享 E2 Host 准入／有效参数和 A2 工具结果视图修复不扩大输出或授权。原四条 wiring 真实数据行与新增 managed Dispatcher 断言仍需当前 Native；旧 O1 定向矩阵只保留原身份，后续当前 O2 联合范围承担共享修订覆盖。

## 代码身份与分平台接续

本次 restack 仅增加 `Q_OS_WIN` 条件内的六行测试清理，产品代码、全部原断言及 Linux 适用业务代码未变；删除该精确插入块可还原前一 Git blob。当前代码 HEAD 不等于下列每份旧实跑收据的 HEAD；不宣称 Linux 全量重跑、Native 重跑或 ELF 调试行号元数据相同。

- 原完整 Linux 构建／ordinary：`828f353f1e42934aab020fc76a33e1bf08ae4771`，保持原 b047／6096 入口及下表范围。
- 后续 alias 单目标增量及已执行 Native 的依据 HEAD：`fef062390f7318d335d6ed99873e52b4335629a6`；其 source/raw/SDK 和失败历史不改标为当前 Windows-only HEAD。
- Linux ort alias 增量：`tst_pythonanalysiswiring` actual 0，raw `3d64c9418db62ce2334409e396b4589fa7e930b9442757e99bd87768fdd2fc91`；ordinary／GUI 未重跑，其余 ELF 按原收据的 stat unchanged 保留。
- Linux noort alias 增量：`tst_pythonanalysiswiring` actual 0，raw `3d64c9418db62ce2334409e396b4589fa7e930b9442757e99bd87768fdd2fc91`；ordinary／GUI 未重跑，其余 ELF 按原收据的 stat unchanged 保留。

本次测试 TU 的 Git LF SHA `2a4d02416cee602a553e0fce29b036d4ddd8cebdb670ab4ba5b759be7490d35c` 与 Windows live CRLF SHA `d3c19285c6427b9c65713d71dd3e55b6d50aa0f707a1074aa0d25d3cfe742ead` 分列；whole-tree raw 只取实际构建／运行收据，计划派生值不当作实跑身份。

原完整构建／ordinary（保持当时 identity）：

| 原平台与配置 | 原已证结果 | 原边界 |
|---|---|---|
| Linux／WSL 默认 | 完整 **79** 目标构建退出 0；10 个 ordinary 套件，Qt **156/0/0**；Office／WebEngine／first_paint／startup_screen_closed 通过 | WSL 精确拒绝 **3/0/0**，不是强执行正例 |
| Linux／WSL no-ORT | 完整 **78** 目标构建退出 0；9 个 ordinary 套件，Qt **142/0/0**；同范围 GUI／组件检查通过 | WSL 精确拒绝 **3/0/0**，不是强执行正例 |

## Windows 实际验收与分域范围

- Windows ort：当前 HEAD 完整 78 目标构建及最终运行身份核验完成。
  当前 final raw `d26118fc2665e5dc7384e3dde1e21d55f44dd6f39ef6b7a5706434884cccc8ea`；ordinary 26 次 Qt 323/0/2/0，实际 exits [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]，allGreen=True；强执行 5 次 Qt 21/0/0/0，首帧 exit 0。各阶段原 HEAD 保存在索引，不统一改标。
- Windows noort：当前 HEAD 完整 77 目标构建及最终运行身份核验完成。
  当前 final raw `d26118fc2665e5dc7384e3dde1e21d55f44dd6f39ef6b7a5706434884cccc8ea`；ordinary 25 次 Qt 303/0/2/0，实际 exits [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]，allGreen=True；强执行 5 次 Qt 21/0/0/0，首帧 exit 0。各阶段原 HEAD 保存在索引，不统一改标。

本轮 E3 至 O2 Windows Runtime 分域只把 `changedFixtureIsReauthenticatedBeforeStrongExecution` 留在既有 strong Native 域；ordinary 保持其余 17 个方法原顺序，包括 `invalidManifests` 的八条数据行。两域为原 18 方法的精确不交并集，原 strong 同函、断言、fixture、REQUIRE、预算均不减。分区本身不计运行结果，实跑状态仅取本轮对应收据。

Windows Qt 四项依次为 passed／failed／skipped／blacklisted；其他原三项按原口径。次数含各次 init/cleanup；不把 direct QtTest 记成 CTest 全绿，也不合并原失败与复跑以伪造首轮 0。

## 已执行 Native：保留真实版本与范围

本批自身 Native 没有本轮重新跑完整矩阵；只引用其原 head/raw/Pi tuple，以及 O2 联合验收的真实 scope，不声称逐批新 HEAD 全量 Native。

统一索引中的五路完整 Native 共 140 项、Qt 1539/0/0，另列上述默认显式 composite；所有版本、scope 与首次失败均保留。Windows-only 条件块不改变 Linux 业务有效代码，因此没有再次启动机器验证或产品运行。

## 更早历史 Native：保留原身份

以下仅为旧十二路交接 `652151751c79022b6764faa830290a81b7579f0ed2946eb389aedaea882b6ff4` 的本批两路，**不是文首新 HEAD 的 Native 结果**。原 head `e3a8281a412bec12ece01892022b6f72f62a7ab7`，SDK `1362a69e61a663aba2b5024bfa83627af0759354`，raw `f633e4c09972ccbd2aacca3f3e321d27b08d452868241ad33d79b574ffd008fa`；旧汇总明确 `newDispatcherSurfacePatchNotCovered=true`。

| 原配置 | 实际执行范围 | Qt passed/failed/skipped | run 秒 |
|---|---|---|---:|
| 默认 | 3 C++ + 8 source gates＝11 项 | 45/0/0 | 88.048 |
| no-ORT | 3 C++ + 8 source gates＝11 项 | 45/0/0 | 123.491 |

上述两份本批 summary 各自核定以下 Pi tuple，一致性来自本批原件核对，**没有统一套用 O2 身份**：

- root：`/home/kevin/paleo-pi-a2-store/generations/ce74ce4fe0c5e26a07f673c65a4a3c27a559c18f806f3e2b6f32fbb58aaf4374`。
- generation／manifest SHA：`ce74ce4fe0c5e26a07f673c65a4a3c27a559c18f806f3e2b6f32fbb58aaf4374`。
- sourceLock SHA：`966289527db156efa006db7d93e00d1c9e1a482473e36a45f1942949faaec79c`。
- 每路的 9 个 Pi 文件及 runtime 均按原 before／after／final 原件保持集合、SHA 与只读；六项 ABI、首帧按旧记录通过。当前没有重读这些正文或重跑该旧矩阵。

旧默认 summary：`/mnt/c/Users/wangj.KEVIN/projects/paleo-python-delivery-e3-20261115/local/stack-validation/native-window-16rounds-20261116/related-09-o1-ort/summary.json`，SHA `e29168cef5ef58aadc650fa52859c2df407e7b01297547c5fb0e6f1f1626d0b2`；旧 no-ORT summary：`/mnt/c/Users/wangj.KEVIN/projects/paleo-python-delivery-e3-20261115/local/stack-validation/native-window-16rounds-20261116/related-10-o1-noort/summary.json`，SHA `16ac3856b4739f5bc8d12a837673d8ffb47ce3bd5d2f91cbbe59c71d301f7e21`。制包／传输／运行／收集和首次失败按各自原索引保留，不把重试或新机器身份写回这些记录。

## 边界与证据

外部分发入口仍关闭；Shell、任意 CLI／SQL／HTTP、在线安装包与持久解释器仍后置。prepare／compute 只产生私有成果，publish 独立；生成数值与像素未经宿主独立重算。Pi 是可信可选编排组件，不是 OS 隔离沙箱；缺强后端不 fallback。Office／WebEngine 与 offscreen 首帧不扩写为 Office WASM DOM 验收。

证据索引位于操作者本地 E3 worktree 的 ignored 路径 `local/final-execution-20261116/final-docs-20261117-r2/reconciled-progress/evidence-index.json`，未入仓库，也不是本进度文档所在目录的仓库文件；对外审阅需另附实际可访问的证据附件或链接。该本地索引分列原完整构建／ordinary、alias 增量、各 Native 原身份与 composite、新 Windows-only proof、有限 O2 Linux crosscheck、动态 revision4 completed/final、原失败及四份 reviewed 全局文档 manifest。原 writer、原输入及生产文档／源码／refs 不修改。

本批分支 `codex/python-table-output-o1-20261115`，直接父分支 `codex/python-seismic-input-i2-20261115`；共同 master 基底为 `e995a477`。跨仓前置为 [Data_service #23](https://github.com/changyanyanchang/Data_service/pull/23)（依赖 #22），SDK `1362a69e`。 实际审阅 HEAD、直接父 PR 及 fetch/rebase 后的 Source gates 结果以本批 PR 正文为准；运行证据继续保留本文列出的实际代码身份，文档提交不重标原实跑版本。


## Windows 队列终态收口范围

E2 至 O2 的十六个 profile 已有真实 build／final 与独立 audit 收据；E2 默认仍按旧全量加新单 TU 的组合等原范围记录，不把原失败或旧实跑版本改标。队列终态以新核对命令实际 exit 0 的 `terminal_reconciliation` 纯元数据收口为准；原工具 session 87628 的 outer exit 因 session 失效仍为未知，不能写成原 outer 0。18823／81832 的原 exit 1 永久保留；本次收口未重新运行 C++、测试或依赖大树 SRI。owner 原件、精确 scope、各旧退出事实与 SHA 均链入上述操作者本地 ignored 证据索引。
