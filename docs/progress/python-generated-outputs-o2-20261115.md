# O2：REPORT／CSV／PNG 与 Pi／SDK 联合收口

日期：2026-11-17。当前代码 HEAD `0aab9d5f7502cf1c91c856072e3ae0b20b58dd75`，SDK `1362a69e61a663aba2b5024bfa83627af0759354`。源码版本、实跑版本及精确范围分列。

## 范围与用户链路

独立 `python.generated_outputs` 明确选择 numeric_table 或 seismic_trace，复用 I1/I2 各自的 SDK selector、认证、InputBundle 与 validator。prepare 同时冻结 code、输出 table、RGBA image、编码身份和 policy；强后端 compute 生成私有 REPORT／TABLE／IMAGE 三端口完整组，用户认证分页查看后独立 publish，经原 outputset／Catalog／receipt／recovery 链原子登记。没有第二个解释器、任务队列、成果库或 publisher。

frame v3 只接受宿主冻结声明一致的 REPORT、类型化 TABLE 和 tightly-packed straight RGBA8。宽高各 1–1024，尺寸乘法与精确字节数先于图像分配；不接受子进程编码的 PNG/SVG/HTML/PDF、路径或 MIME。宿主编码 CSV 和 PNG，REPORT≤256 KiB、CSV/PNG 各≤4 MiB，原始与最终组还服从 12 MiB 和更小 policy 预算；最大尺寸不承诺任意不可压缩 PNG 都能装下。

compute/resume 核当前 Qt 编译／运行时和固定 encoder 身份，漂移明确拒绝；publish 既有认证 PNG 只消费历史字节，不要求当前编码器或重新执行。第三个私有文件写失败、坏端口、取消或预算错误均不返回部分成果。PNG 是**非空间报告图片**，没有 CRS/extent，**不自动创建 GIS 图层**，不表示已接受的地质解释；同 binary 恢复不冒称已实跑跨 Qt 升级。

本次注册 `python.generated_outputs`、对齐声明名，并继承 E2 命令／Host 准入与 A2 工具结果视图夹具修订；命令可发现不等于授权。对应契约为 `docs/PYTHON_GENERATED_OUTPUTS.md`、`vendor/agent-python/{NUMERIC_OUTPUT,IMAGE_OUTPUT}.md`。联合 Native 采用下表所列实际 scope；不以历史矩阵替代。

## 代码身份与分平台接续

本次 restack 仅增加 `Q_OS_WIN` 条件内的六行测试清理，产品代码、全部原断言及 Linux 适用业务代码未变；删除该精确插入块可还原前一 Git blob。当前代码 HEAD 不等于下列每份旧实跑收据的 HEAD；不宣称 Linux 全量重跑、Native 重跑或 ELF 调试行号元数据相同。

- 原完整 Linux 构建／ordinary：`e85e6ec05de8bfd8a53adf9b9a01816320382b04`，保持原 b047／6096 入口及下表范围。
- 后续 alias 单目标增量及已执行 Native 的依据 HEAD：`99b84125bca2ca71774cbb9ff022ab59b3a8a2f6`；其 source/raw/SDK 和失败历史不改标为当前 Windows-only HEAD。
- Linux ort alias 增量：`tst_pythonanalysiswiring` actual 0，raw `150a56efac2675dc3c5c72b539c71d539ce8b1012b7f9dae989f6180ffd602a7`；ordinary／GUI 未重跑，其余 ELF 按原收据的 size/mtime equivalent to original baseline 保留。
- Linux noort alias 增量：`tst_pythonanalysiswiring` actual 0，raw `150a56efac2675dc3c5c72b539c71d539ce8b1012b7f9dae989f6180ffd602a7`；ordinary／GUI 未重跑，其余 ELF 按原收据的 stat unchanged 保留。

本次测试 TU 的 Git LF SHA `2a4d02416cee602a553e0fce29b036d4ddd8cebdb670ab4ba5b759be7490d35c` 与 Windows live CRLF SHA `d3c19285c6427b9c65713d71dd3e55b6d50aa0f707a1074aa0d25d3cfe742ead` 分列；whole-tree raw 只取实际构建／运行收据，计划派生值不当作实跑身份。

原完整构建／ordinary（保持当时 identity）：

| 原平台与配置 | 原已证结果 | 原边界 |
|---|---|---|
| Linux／WSL 默认 | 完整 **83** 目标构建退出 0；10 个 ordinary 套件，Qt **156/0/0**；Office／WebEngine／first_paint／startup_screen_closed 通过 | WSL 精确拒绝 **3/0/0**，不是强执行正例 |
| Linux／WSL no-ORT | 完整 **82** 目标构建退出 0；9 个 ordinary 套件，Qt **142/0/0**；同范围 GUI／组件检查通过 | WSL 精确拒绝 **3/0/0**，不是强执行正例 |

新 O2 `0aab9d5f` 另有有限 Linux 交叉检查：默认／无 ORT 仅重编该测试，ordinary 六函数各 exit 0、Qt 8/0/0/0；raw `b28e3d3ae0ba86ea22a19c6f8935ba38e1f0d3c7d339e7b6f5da760a5ebc3dbc`。旧源、marker 和旧 ELF 保留；没有重跑 Native、GUI、广泛 ordinary 或其余十四配置。

## Windows 实际验收与分域范围

- Windows ort：当前 HEAD 完整 82 目标构建及最终运行身份核验完成。
  当前 final raw `3fbe38e2908950205051875bc9670443e3ecb1660e838f8a0109ebf6a347ae7a`；ordinary 28 次 Qt 371/0/2/0，实际 exits [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]，allGreen=True；强执行 5 次 Qt 24/0/0/0，首帧 exit 0。各阶段原 HEAD 保存在索引，不统一改标。
- Windows noort：当前 HEAD 完整 81 目标构建及最终运行身份核验完成。
  当前 final raw `3fbe38e2908950205051875bc9670443e3ecb1660e838f8a0109ebf6a347ae7a`；ordinary 27 次 Qt 351/0/2/0，实际 exits [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]，allGreen=True；强执行 5 次 Qt 24/0/0/0，首帧 exit 0。各阶段原 HEAD 保存在索引，不统一改标。

本轮 E3 至 O2 Windows Runtime 分域只把 `changedFixtureIsReauthenticatedBeforeStrongExecution` 留在既有 strong Native 域；ordinary 保持其余 17 个方法原顺序，包括 `invalidManifests` 的八条数据行。两域为原 18 方法的精确不交并集，原 strong 同函、断言、fixture、REQUIRE、预算均不减。分区本身不计运行结果，实跑状态仅取本轮对应收据。

Windows Qt 四项依次为 passed／failed／skipped／blacklisted；其他原三项按原口径。次数含各次 init/cleanup；不把 direct QtTest 记成 CTest 全绿，也不合并原失败与复跑以伪造首轮 0。

## 已执行 Native：保留真实版本与范围

- O2 默认为显式 composite：原 `e85e6ec0` 广泛 87 项，86 pass／1 fail、Qt 1105/2/0、run 1 保留；`99b84125` focused 9 项、Qt 12/0/0，四条真实 wiring 行及 GUI／ABI／runtime/Pi 前后完整性通过。原 87 不改绿，也不称新 0aab 完整重跑。
- Native noort：实际 `99b84125bca2ca71774cbb9ff022ab59b3a8a2f6`，raw `150a56efac2675dc3c5c72b539c71d539ce8b1012b7f9dae989f6180ffd602a7`，86 项、Qt 1087/0/0；run／collect／CTest、六 ABI、首帧和前后完整性按该原身份通过，不重标当前代码 HEAD。

统一索引中的五路完整 Native 共 140 项、Qt 1539/0/0，另列上述默认显式 composite；所有版本、scope 与首次失败均保留。Windows-only 条件块不改变 Linux 业务有效代码，因此没有再次启动机器验证或产品运行。

## 更早历史 Native：保留原身份

以下仅为旧十二路交接 `652151751c79022b6764faa830290a81b7579f0ed2946eb389aedaea882b6ff4` 的本批两路，**不是文首新 HEAD 的 Native 结果**。原 head `ce0a0e5709a9c2fdbb3a1f83f0358e410903fa7b`，SDK `1362a69e61a663aba2b5024bfa83627af0759354`，raw `594a0cb04fbed9e0e53a125a6295c8637b1dfd59b0d734a4b55b19e91b3cd446`；旧汇总明确 `newDispatcherSurfacePatchNotCovered=true`。

| 原配置 | 实际执行范围 | Qt passed/failed/skipped | run 秒 |
|---|---|---|---:|
| 默认 | 76 C++ + 8 source gates＝84 项 | 1073/0/0 | 858.689 |
| no-ORT | 76 C++ + 8 source gates＝84 项 | 1067/0/0 | 689.749 |

上述两份本批 summary 各自核定以下 Pi tuple，一致性来自本批原件核对，**没有统一套用 O2 身份**：

- root：`/home/kevin/paleo-pi-a2-store/generations/ce74ce4fe0c5e26a07f673c65a4a3c27a559c18f806f3e2b6f32fbb58aaf4374`。
- generation／manifest SHA：`ce74ce4fe0c5e26a07f673c65a4a3c27a559c18f806f3e2b6f32fbb58aaf4374`。
- sourceLock SHA：`966289527db156efa006db7d93e00d1c9e1a482473e36a45f1942949faaec79c`。
- 每路的 9 个 Pi 文件及 runtime 均按原 before／after／final 原件保持集合、SHA 与只读；六项 ABI、首帧按旧记录通过。当前没有重读这些正文或重跑该旧矩阵。

旧默认 summary：`C:\Users\wangj.KEVIN\projects\paleo-python-image-output-o2-20261115\local\lint-2723228a-20261116\native-v2\ort-native-summary.json`，SHA `311b7163aaae1a1d4dabe82f30e2090b29a8e93c07017d50956098b78cbc7de4`；旧 no-ORT summary：`C:\Users\wangj.KEVIN\projects\paleo-python-image-output-o2-20261115\local\lint-2723228a-20261116\native-v2\noort-window-summary.json`，SHA `ee6866748da1983a0e82304c0c02e1beff49b40261f6e72b821ddcf248b3477c`。制包／传输／运行／收集和首次失败按各自原索引保留，不把重试或新机器身份写回这些记录。

## 边界与证据

外部分发入口仍关闭；Shell、任意 CLI／SQL／HTTP、在线安装包与持久解释器仍后置。prepare／compute 只产生私有成果，publish 独立；生成数值与像素未经宿主独立重算。Pi 是可信可选编排组件，不是 OS 隔离沙箱；缺强后端不 fallback。Office／WebEngine 与 offscreen 首帧不扩写为 Office WASM DOM 验收。

证据索引位于操作者本地 E3 worktree 的 ignored 路径 `local/final-execution-20261116/final-docs-20261117-r2/reconciled-progress/evidence-index.json`，未入仓库，也不是本进度文档所在目录的仓库文件；对外审阅需另附实际可访问的证据附件或链接。该本地索引分列原完整构建／ordinary、alias 增量、各 Native 原身份与 composite、新 Windows-only proof、有限 O2 Linux crosscheck、动态 revision4 completed/final、原失败及四份 reviewed 全局文档 manifest。原 writer、原输入及生产文档／源码／refs 不修改。

本批分支 `codex/python-image-output-o2-20261115`，直接父分支 `codex/python-table-output-o1-20261115`；共同 master 基底为 `e995a477`。跨仓前置为 [Data_service #23](https://github.com/changyanyanchang/Data_service/pull/23)（依赖 #22），SDK `1362a69e`。 实际审阅 HEAD、直接父 PR 及 fetch/rebase 后的 Source gates 结果以本批 PR 正文为准；运行证据继续保留本文列出的实际代码身份，文档提交不重标原实跑版本。


## Windows 队列终态收口范围

E2 至 O2 的十六个 profile 已有真实 build／final 与独立 audit 收据；E2 默认仍按旧全量加新单 TU 的组合等原范围记录，不把原失败或旧实跑版本改标。队列终态以新核对命令实际 exit 0 的 `terminal_reconciliation` 纯元数据收口为准；原工具 session 87628 的 outer exit 因 session 失效仍为未知，不能写成原 outer 0。18823／81832 的原 exit 1 永久保留；本次收口未重新运行 C++、测试或依赖大树 SRI。owner 原件、精确 scope、各旧退出事实与 SHA 均链入上述操作者本地 ignored 证据索引。
