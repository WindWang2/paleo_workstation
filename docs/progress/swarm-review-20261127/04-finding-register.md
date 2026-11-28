# Swarm review 2026-11-27 — 发现登记

基线 `42296fd3`（master，2026-11-27）。八路只读审查，主会话抽查了全部 P1
和下面点名的 P2 源码。没有改生产代码，没有跑构建或测试。

覆盖边界：不是 2100 个 C++ 文件逐行。每路先按缺陷模式扫描，再读热路径。
完整证据在 `findings/agent-*.md`。已知未完成迁移（TODOS.md 的文档模型、
剩余 Agent 命令、Python shell/网络）没有重报。

| 分片 | 范围 | 条数 |
|------|------|------|
| A | io / metadata | 6 |
| B | domain / catalog | 5 |
| C | algorithms | 2 |
| D | services | 6 |
| E | workflow / linkage（不含 Agent 宿主） | 7 |
| F | ai + Agent 宿主 | 4 |
| G | qgis | 6 |
| H | ui / app | 6 |

严重度（主会话改过一处）：P1 × 5，P2 × 28，P3 × 7，P4 × 2。
E-01 从 P1 降为 P2：`QSaveFile::write() < 0` 漏掉短写，但同函数已经检查
`commit()`，短写不是这条路径的常态失败。

## P1（主会话复核）

| ID | Conf | 位置 | 行为 |
|----|------|------|------|
| B-01 | VERIFIED | `src/catalog/areawellprojection.cpp:150` | 工区井登记在模型 id 对不上时按井名绑到旧卡片，并把 `areaModelWellId` 写成新井。卡片 id 和 LAS 链接不动，曲线/分层会读到另一口井。发布恢复和代数落后时的 bootstrap 会走到这里。 |
| B-02 | VERIFIED | `src/catalog/areasourceprojection.cpp:206` | `volumes.name` 没有唯一约束。同名的第二个体被挂到第一张测网卡片上，`areaModelVolumeId` 仍是旧 id，层位链接跟着错卡片走。 |
| G-01 | HIGH | `src/qgis/paleomapfacies.cpp:143` | 相图归一化只捕获 `QgsCsException`，不看 `geometry.transform` 的返回值。转换无效且不抛异常时，源坐标写进目标 CRS 的 GPKG。 |
| H-01 | VERIFIED | `src/ui/pages/mappingworkbenchpage_schema.cpp:202` | `catalogBound` 清掉未保存的微相分类草稿。`maybeSaveProject`（`paleomainwindow.cpp:899`）只看工程脏标记和地层对比，.qgz 干净时换工程不询问。 |
| H-02 | VERIFIED | `src/ui/datapreview/datapreviewtabs.cpp:167` | 预览页关闭和换工程直接销毁井预览。TOPs 编辑只在「保存派生」时落盘，关闭路径和 `maybeSaveProject` 都不看这个脏标记。 |

## P2（按主题）

身份与坐标

- B-03 `areasourceprojection.cpp:118`：井名归一化把 `A-1` 和 `A1` 合成一把钥匙，坐标写到另一口井或被跳过，调用仍返回成功。
- G-02 `qgiscanvascontroller.cpp:276`：缩放到图层时，画布 CRS 不可达就保留图层自己的坐标。
- G-03 `seismicsectiontool.cpp:63`：转换失败时剖面吸附丢掉全部井，工程网格画布也一样。
- G-04 `qgislayerservice.cpp:302`：无 CRS 图层被标成 EPSG:32650 和「已确认工程 CRS」。
- G-05 `crossplotmaplink.cpp:38`：交会图定位在没有转换时用样品坐标对画布。
- G-06 `mapfeaturepicker.cpp:32`：转换失败时用图层坐标对画布半径做拾取。

写入与索引

- A-01 `wellfileparsers.cpp:494`：流式井表把截断的 GBK 尾字节解成 U+FFFD。
- A-02 `welltable.cpp:291`：深度解算重写顶界表时丢掉底界行。
- A-03 `dataimport_session.cpp:169`：受管原始副本最后一次 flush 失败仍报成功。
- A-04 `constraintstore.cpp:580`：约束审计复制把坏页当成历史结尾。
- A-05 `wellfileread.cpp:9`：预览和工程读井表不走导入的尺寸预算。
- D-01 `evidenceindex.cpp:271`：GDAL 打不开时仍记下 sha，且 `truncated` 为假。下一轮同步在 `:655` 因 sha 未变跳过，空索引不再重试。主会话对上了跳过条件。
- D-02 `evidenceindex.cpp:276`：`source_suffix` 点名的图层不存在时改索引第 0 层，并当成完整索引入库。
- D-03 `evidenceindex.cpp:462`：`Index::open` 失败不 `removeDatabase`，工作台每次重试再留一条连接。
- D-04 `seismictaskservice_attr.cpp:49`：SEG-Y 延迟读失败写成 0 ms，时间切片和属性体按这个原点落盘。
- D-05 `seismictaskservice_transcode.cpp:628`：计划里去掉的 LOD 文件删不掉仍返回成功，下次可能打开旧的粗级振幅。
- E-01 `stratigraphyworkflow.cpp:282`：应用地层界面写 `project_area.json` 时，`write() < 0` 放行短写，随后 `commit()` 成功并提示已应用。
- E-03 `inversionworkflow.cpp:414`：子波 JSON flush 失败仍作为版本提交。
- E-04 `evolutionworkflow.cpp:415`：演化指标 CSV 未检查 flush 就登记。
- H-03 `wellsectionpanel.cpp:945`：连井 SVG/PDF 导出不看设备写入结果，失败没有对话框。
- H-04 `seismicsectiondock_internal.h:31`：地震拾取自动保存忽略失败的 commit，换工程后拾取丢失。
- H-06 `llmdiagnosticspanel.cpp:182`：诊断导出短写仍提示已导出。

计算、任务、Agent

- C-01 `kriging.cpp:239`：`maxPoints <= 0` 时邻域取全样本集，矩阵预算闸被绕开。Agent 原子的 K 在 1..64，不走这条。
- E-02 `unitonnxcontract.cpp:101`：取消地层单元地震窗读取被记成 `seismic_source_unavailable`。
- F-01 `remotepredictrouter.cpp:216` 与 `:246`：健康检查和预测都没有 `ManualRedirectPolicy`。同仓库的 `llmclient`、井相服务和模型清单都有。Qt 6 默认会跟随 https 跨主机重定向，预测体会把样品送到重定向目标，并把对方的 200 网格当成相结果。主会话确认这两处请求都没设该属性。
- F-02 `aichatcontroller_toolloop.cpp:192`：工具结果放不进缓存时，只有 `paleo.read_result` 的额度变化会把 ok 改成失败。其它成功调用仍对模型和 Pi 报成功，载荷已经删掉。
- F-03 `geostatictools.cpp:48`：克里金 / SGS 的 `parameters` 在 schema 里必填，描述却让 compute-only 省略。发布路径（`capabilityhost.cpp:214`）又拒绝非空 `parameters`。按说明去发布会被 schema 或宿主拒掉。原子解码器对到达它的 publish 直接返回 `publish_requires_source_operation`（`geostatatomcontract.cpp:655`）。
- H-05 `paleomainwindow_sections.cpp:299`：换剖面绘制工具后，设置对话框保持隐藏。

## P3 / P4

- P3：A-06 有上限的整文件读接受短读；B-04 同名顶界取第一条 MD；B-05 非单调 GR 把岩性段底界写到邻段；D-06 取消井相输入加载报成哈希不符；E-05 自动解释队列丢掉最旧请求仍报已排队；E-06 解释侧车写失败仍显示草稿就绪；F-04 `capability_inspect` 把会写目录栅格的克里金/SGS 描述成 Read。
- P4：C-02 约束克里金方差把已经还原的 λ·γ 再乘行缩放（当前无生产调用方）；E-07 `batchjobqueue.cpp:707` 损坏的批次状态续跑成全新成功（无生产调用方）。

## 复核后未立条

- 同层任务 busy 泄漏（2026-10-28 F-01）已按 task id 释放，`paleotaskservice.cpp:361`。
- `class_index` 被通道数误杀（F-24）在 `tileinference.cpp:124` 已按 255 类号上限判断。
- `LlmClient` 不跟随重定向，空 SSE 不报成功。
- 克里金/SGS 原子在 `finite_cells <= 0` 时失败，不发布空栅格（`geostatatomoperation.cpp:278`）。
- 会话 id 拒绝 `..`；operation 私有目录拒绝越出工程根的符号链接。
- 已启用的 Python 沙箱（Linux bwrap+seccomp，Windows LPAC、无网络）没有新的逃逸洞。Shell、在线安装、持久解释器仍是 TODOS 里的延期项。
- #1117 的 LAS 改挂井筒检查在 `documentsession_wellbores.cpp`，本轮没有发现同一函数里的残留洞。B-01 是另一条登记路径。

## 修复（分支 `codex/swarm-review-20261127`，已 rebase 至 `d682028b`）

42 条都已改代码并完成 Windows Release 编译与回归验证。P1 的落点：

- B-01 / B-02：井卡片先按 `areaModelWellId` 更新，测网只按体积 id 匹配。
- G-01：相图归一化在转换无效或 `geometry.transform` 非 Success 时删掉半成品并失败。
- H-01 / H-02：`maybeSaveProject` 在干净工程提前返回之前询问微相草稿、TOP 编辑和未落盘的地震拾取。关闭单个预览页单独询问。换工程时 `closeAllTabs` 不再问第二次。

同一分支还包括其余 P2–P4，以及随 rebase 与本地验证补齐的收口项：`attachSections` 抽出 `wireWellCompositeAlignmentSink`（`tools/file-budget-baseline.txt` 同步下调 `260 → 240`）、`translations/paleo_{zh_CN,en}.ts` 补齐 20 条新增 `tr()`、`paleoReplaceFile` Windows 并发替换增加 `ERROR_SHARING_VIOLATION` / `ERROR_LOCK_VIOLATION` 有界重试，以及对应单测修正（`tst_wellfileparsers`、`tst_areamodel`、`tst_layerservice`、`tst_seismicattrscan`、`tst_wellsection`）。

本地验证（2026-11-28，Windows Release，受控 OSGeo4W/ORT/Pi/Office 依赖，因仓库目录带 `Low Mandatory Level` ACL 将产物暂存到 `C:\tmp\paleo-swarm-run` 执行）：`paleo.exe`、`paleo_selfcheck.exe`（725 ms）及 35 个受影响/相关测试套件全部通过（0 失败）；全量 Source gates（含 `--transitive --qgis-include`、`--qgs-audit --strict`、`--strict` 分层/文件预算/UI 门禁、`check_ts_stale`、`sync_en_ts_skeleton`、`check_pi_agent`、`check_test_manifest --require-ort`、`verify-cpp-mainline.cmake` 等）全部通过。未覆盖项：未在此 Windows 本机运行 Linux 专属车道及非受影响测试全集。

## 建议的修复顺序

1. B-01、B-02：井和测网按模型 id 绑定，名字只做展示，冲突则失败而不是并卡。
2. H-01、H-02：换工程前提示微相草稿和 TOPs 编辑，或把它们算进 `maybeSaveProject`。
3. G-01 以及 G-02/G-03/G-05/G-06：转换无效就失败返回，不要把源坐标写进目标 CRS。
4. F-01：远端预测与健康检查改为手动重定向。
5. F-03：克里金/SGS 的 schema、描述和宿主对 `parameters` 使用同一套发布规则。
6. D-01、D-02：证据索引在打开失败或图层名未命中时不要把 sha 记成完整。
