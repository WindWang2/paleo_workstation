# W6：效果、冻结推理与生产取数

按 [W0–W6 计划](../planning/AGENT_CALL_READINESS_PLAN.md) 实施，接在 W3、W4 和 W5 之后。
此批迁移已有工具，不扩大为任意脚本执行、全命令 Agent 化或跨 store 全局事务。
交付 [PR #941](https://github.com/WindWang2/paleo_workstation/pull/941)，已于 2026-11-14 合入 master `4fee95dd`。

## 入口和效果

- 工具静态描述、当前 readiness 和宿主效果授权分别检查。生产工具列表按当前状态过滤；
  历史调用仍返回具体不可用原因。模型提供的 JSON 不能授予效果、调用者身份或工程路径。
- `paleo.tile_classification_v2` contract 2 分 `prepare`、`compute-only`、`publish`。
  前两阶段仅写受管操作私有目录；publish 一次登记三端口输出集，随后在 owner 线程呈现。
  旧版 tile 入口仅保留明确授权的内部适配，不能经生产聊天绕过新契约。
- tracking 使用真实 SDK 体引用与解释上下文，读取准确 IL/XL 和有界样点窗；建议本身不接收
  或发布地质解释。缺体、线号、时间单位或基准分别拒绝，不用减最小线号猜 trace ordinal。
- well-facies contract 2 新预测需要 `Read | PrivateStage | ExternalTransfer`，
  模型 GET 为 `Read | ExternalTransfer`，完整持久回执回放仅需 `Read`。
  实际有效的内存端点/密钥才产生传输授权；空值、CR/LF 密钥和无效 URL 不获授权。
  无端点时在任务受理前返回具体缺项，不先报笼统权限不足。

## tile 的冻结与提交

prepare 冻结 Catalog 精确版本、完整栅格来源包、网格/CRS、特征契约、模型字节 SHA、
真实输入输出形状/语义与 Spec 参数。compute 不重新解析 live 图层，不登记 Catalog 版本或
声明；publish 只消费指定已计算操作，不重跑模型。旧请求键先按可信文档/聊天作用域回放，
已提交回执不依赖当前模型在线。

当前输入契约为单波段 GTiff、单通道原始 float32 特征。上限为边长 32768、总像素 1M，
来源主文件及最多 15 个同目录已消费 sidecar 合计 512 MiB；mask 也纳入指纹。
特征的 kind/unit/domain/preprocessing 必须明确；声明相互矛盾、未知语义或未支持的变换
在 prepare 拒绝。单位不猜测，`mV` 与 `MV` 不合并。Double NoData/有效性先判定再转 float；
Int64/UInt64/complex 当前明确不支持，避免精度损失被隐藏。

模型主文件上限 512 MiB，新会话从同一份已验证内存字节构造 ORT session。外置 initializer
尚未形成完整权重包契约，因此明确拒绝。实际 ORT 输出需为支持的 float32/rank4/N=1/
类别数契约。静态超预算在 warmup/Run 前拒绝；实际首输出在 QVector resize/memcpy 前
检查 32M 元素上限，不依据 manifest 中较小的声明忽略真实输出。这不是动态 graph 内部
申请的通用沙箱，详见[输出预算记录](agent-onnx-output-budget-20261114.md)。
概率输出必须在范围内且归一化，不能悄悄裁剪。

相码 OUTPUT、低置信 MASKED、CONFIDENCE 是三个必需端口，同一 Catalog commitSet 落库，
沿用 W5 同事务凭证、目标 CAS 和幂等恢复。取消发生在 owner 接收 prepared 回调时仍须先
持久化，再决定是否发布；已提交后取消保留成果与提交事实。

呈现同时比较 LayerManifest 和真实原生图层的名称、来源、CRS、QML 状态。自己的 baseline
在外部图层信号前捕获，放在 QgsProject entry 中随 qgz 保存，不参与 QML 自哈希。
用户改名/renderer/透明度或后续操作更改目标后，恢复 A 标记 superseded，不覆盖目标。
自身部分成功的重试不重建已正确呈现的实例，不额外新增版本；已释放的层位继续按需实例化。
呈现失败保留 committed 与 pending，重试受次数/时间政策限制。

## 查询模型与实际取数

`query_project(topic=local_models)` 读取已安装 registry 的有界元数据页，游标绑定可信文档、
聊天、epoch、查询和字节预算。没有运行模型或为查询加载权重；`manifest_status_ok` 仅表示
声明状态，返回 `metadataOnly=true`、`requires_prepare=true`、`runtimeVerified=false`。
缺输出语义/特征的声明仍可查询，prepare 按支持契约拒绝。无 ORT 构建的这个主题明确
返回 `ort_unavailable`；元数据可读不代表模型或运行库已经验证。

well 的模型 GET 与输入元数据入口分别为 `well_facies_models` 和 `well_facies_inputs`。
模型 GET 需要外部传输效果，只证明模型契约，不读取井曲线。没有意义的续页参数明确拒绝，
不能忽略 cursor 让调用者误以为续读成功。

well 取数先冻结模型版本、端点身份、特征顺序/单位、窗口和标签 schema，再用 WellRef、
ContextKey/UnitScope、实际曲线/source/logSet ID 及消费值快照/hash 组装输入。
当前严格取数仅支持明确声明的 MD（m/ft）；TVD/TVDSS 保留其语义并返回
`input_depth_mapping_unavailable`，不假装已经完成深度映射。curveRevisionMode 明确为
frozen delivered values，model_head 只做读取一致性水位。离散 occurrence 有稳定身份，
不把多个区间合成 MIN/MAX 大窗；未知单位/基准/曲线或歧义区间明确拒绝。
生产接线从 SDK 读取，不依赖打开的 widget 缓存。详细协议与独立验证见
[well 记录](agent-well-facies-20261114.md)。

SDK 依赖按顺序为 [PR18](https://github.com/changyanyanchang/Data_service/pull/18)、
[PR19](https://github.com/changyanyanchang/Data_service/pull/19)、
[PR20](https://github.com/changyanyanchang/Data_service/pull/20)，最终 pin `8c0a329`。
指定井的曲线/分层道按 `(well_id,id)` 索引 seek，不先扫描其他井或执行相关 COUNT。
旧只读库缺派生索引时明确 unavailable，可写打开在既有事务内重建。真实 SQL next() 错误
整页失败，不能返回部分成功。

## 外部任务与恢复

父输入快照只是 `execution=prepared`、computeCount=0；每段推理有独立持久子操作。
汇总使用 `executionScope=batch_summary` 和 inputOperationId，不能拿输入快照冒充整体成功。
冻结输入与结果缓存使用受管私有目录并纳入 GC 根；结果读取校验 containment 和 SHA。

发送前持久化意图，拿到 job ID 立即保存。恢复只 GET 原 job；无 job ID 且结局未知时返回
ExternalOutcomeUnknown，不盲目 POST。已知完成结果先保存 content-addressed 字节及完成证明，
再附加操作工件；聊天取消或换工程后，存活的原 owner 仍能收尾已知事实。进程退出后由
新协调器凭已持久化的完成证明/缓存恢复；无 job 且完成证明尚未持久化时仍为
ExternalOutcomeUnknown，不能推断完成或自动重发。
父取消阻止下一段新 POST，但原已受理 job 仍可 GET 核对并保存完成事实。

同键完整回放只需 Read、可信作用域和原意图摘要，无需当前端点/provider 或工程写权限。
它返回持久执行回执与引用，明确 `reference_unverified` 和 `resultBytesVerified=false`；
实际字节由 operation_read 校验。缓存删除或 transport 预算不足不改写已知业务事实。
W1 的有界兜底也保留输入/子操作指针、执行范围和远端结局。

## 验证

Linux/WSL archlinux，GCC 16.2.1、Qt 6.11.2、锁版 QGIS 4.2.3/GDAL 3.12.2、ORT 1.30.0。
实际生产 `paleo` 与下列全部测试目标完成编译/链接；两种配置的 Office editor 都已就绪。
运行环境使用锁版依赖闭包，`ldd` 没有 missing dependency；默认配置链接 ORT，无 ORT 配置不链接它。

| 配置 | CTest | QtTest（含 fixture） | 失败 / 跳过 | 耗时 |
|---|---:|---:|---:|---:|
| 默认 ORT | 53/53 | 821 | 0 / 0 | 194.23 秒 |
| 无 ORT | 41/41 | 637 | 0 / 0 | 120.06 秒 |

两者均启用 `PALEO_CATALOG_SCALE=10000`；设置独立 `PALEO_AI_RESULT_SHOT_DIR` 并实际保存四张
结果卡 PNG，因此截图用例也运行，不把默认按需跳过记成测试通过。测试串行，团队构建总并行
不超过 8。没有用耗时放宽代替索引/预算证明。

共同 40 个套件：

```text
tst_chatresults tst_aichat tst_aichatcontroller tst_aichatprojectquery
tst_aichatcapability tst_capability_gateway tst_capability_plan tst_capability_round2
tst_capability_host tst_commanddispatcher tst_commandshell tst_document_sources
tst_capability_seismic_input tst_document_relocation tst_project_query tst_wellref
tst_catalogstore tst_datacatalog_sqlite tst_catalog_catalogindex tst_catalogroles
tst_catalog tst_artifact_version_capability tst_derivationgraph tst_aiwiring tst_aitoolresultview
tst_publication_receipt tst_operation_control tst_algorithm_output_set
tst_capability_recovery tst_capability_recovery_native tst_storagegovernance tst_assetops
tst_tracking_input tst_wellfaciesagent tst_wellfaciesinputplan tst_wellfaciescontract tst_wellfacies_remote
tst_algorithm_spec tst_algorithm_contract tst_algorithm_adapters
```

默认配置另加 13 套 `tst_aichattoolloop`、`tst_agent_readiness`、`tst_pinned_inference`、
`tst_tile_operation`、`tst_tile_agent`、`tst_modelcontractquery`、`tst_modelregistry`、`tst_onnx`、
`tst_onnxsessions`、`tst_tileinference`、`tst_aiassistworkflow`、`tst_onnxworkflow`、`tst_ai_horizonsuggest`；
无 ORT 配置另加 `tst_agent_noort`。无 ORT configure 最后显式覆盖
`-DORT_INCLUDE=OFF -DORT_LIB=OFF`，远端测井相与宿主/恢复回归继续运行。

实际命令采用两个独立 build，`tests` 数组为上列相应配置的完整目标集合：

```bash
export PALEO_CATALOG_SCALE=10000
export PALEO_AI_RESULT_SHOT_DIR="$build/agent-result-shots"
mkdir -p "$PALEO_AI_RESULT_SHOT_DIR"
jobs=6 # 默认；无 ORT 最终为 8
cmake --build "$build" --target paleo "${tests[@]}" -j"$jobs"
cmake --build "$build" --target paleo "${tests[@]}" -j"$jobs" # 再扫描，确认没有遗漏编辑
pattern=$(IFS='|'; printf '%s' "${tests[*]}")
ctest --test-dir "$build" -R "^($pattern)$" --output-on-failure -j1
```

build 目录为 `/home/kevin/build-paleo-agent-providers` 与
`/home/kevin/build-paleo-agent-providers-noort`。日志为各自 `final-matrix-{build,tests}.log`、
`final-matrix-counts.json`、`final-runtime-closure.log` 及 `Testing/qtest-first-run/*.txt`；
QtTest 总数从真实文件输出汇总，全部 53/41 个文件都有计数，不以空 stdout 推断为零项。
取证 PNG 在各 build 的 `agent-result-shots/`。

关键真实回归包括：tile 三阶段零领域版本/声明及三件套原子发布、源变化/取消、换宿主和
模型离线回放、原生改名/renderer/同步信号编辑保护、qgz 重开及层位释放；精确冻结模型消失
在调度前拒绝，且 Host 保留注册 restorer 的具体错误，不回落为另一类原生计划错误。
well 的实际 owner/HTTP/硬退出恢复 20 项通过；SDK 四套独立验证见配套记录。

首轮 53 套有六套失败，原始 CTest 日志保存在 `final-matrix-tests-first.log`，
早期 Qt 回归输出在 `Testing/qtest-initial-matrix/`。修复了源夹具定位、QGIS 生命周期/已持锁
重开夹具、旧工具名单/测井相参数断言，以及聊天 publish 枚举遗漏。新的零 Catalog mutation
断言还促成 W5 的 UUID 预留和同名目标 CAS 修复；冻结模型缺失反例促成 restorer 错误保留。
验证脚本取证开关曾在运行中修改，导致一次脚本解析中断；固定脚本后重新完整编译/运行。
这些失败和中断均不计为通过。

开 PR 前 fetch 并以 `git rebase --rebase-merges --no-update-refs origin/master` 同步基线，
保留 W4/W5 的共同依赖关系。master 仍为 `dd7baa1f`；成功后 HEAD/tree 均未改变，
已验证源码没有发生变化。Source gates 用显式锁版 QGIS 头启用 transitive 四指标，
最终 **32/32 通过**，`git diff --check` 通过，所有基线未增长。命令按 CI 的 Source gates
列表逐项执行，另含 qgis_spread 自测、known-reds 校验/自测和原子核检查；日志在 worktree
`local/review-20261113/source-gates/{01..32}.log` 与 `results.json`。

W6 初次审阅基底 `codex/agent-integration-base-20261114@18098285` 仅组合当时待审的
[W4 #939](https://github.com/WindWang2/paleo_workstation/pull/939) 与
[W5 #940](https://github.com/WindWang2/paleo_workstation/pull/940)。后续按 W1→W2→W3→W4/W5
合入，再将 W6 重定位到 master；文件树仍与本记录验收树相同。内部基底未作为额外功能 PR。

本批使用合成 SDK/栅格/ONNX 数据和 loopback HTTP，没有向真实远端发送工区数据。
本机 Windows 全应用构建、真实远端服务验收和全应用视觉验收不属于这份 Linux 运行证据。
端点缺 feature-unit 等契约时仍如实不可用；不承诺远端没有提供的幂等/查询语义。
