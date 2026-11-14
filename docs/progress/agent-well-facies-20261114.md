# W6 测井相生产取数与远端任务恢复

本批把 `paleo.well_facies_prediction` 的生产入口改为 typed v2 外部适配器。
先查询并冻结端点身份、模型实际 id/version、按序特征、窗口、标签及训练单位，再在
PaleoTaskService worker 中读取真实 SDK 值和序列化请求。模型缺项不发送井数组。
CapabilitySpec 标明 v2 LegacyAdapter、可恢复原任务/结果、不可重新启动，不能承诺远端取消回滚；
Native DAG 不接收该适配器。旧按井名回调仅保留显式 trusted legacy 接口。

输入由 WellRef、FaciesAnalysis ContextKey 和固定修订的 UnitScope 定位。
目录井到 SDK 井的映射还由生产装配核对，计划井或挂错实际井编号不能取数。
曲线/岩性道使用实际对象、来源和单位；不据名字推断单位、不把未知 MD 原点当作兼容，
不把 TVD/TVDSS 改标签后冒充 MD。模型单位大小写保留，`mV` 和 `MV` 不等价。
已声明 MD 的 m/ft 转换、SDK null/threshold/manual 质量、采样缺口以及每个离散
occurrence 分别保留，S1–S3/整段半开、S4 有条件保留最后端点，不合 MIN/MAX 窗。
公开 SDK occurrence 身份由独立 Data_service PR19 提供；旧固定修订无该字段时仅使用快照哈希。

全局 model_head 用作查询/读取一致性水位，不充当所有曲线值修订。
输入摘要包含实际消费值、来源、源深度语义、精确模型/分类、各出现窗口和请求 body hash。
读取和 hash 均在 worker；聊天只订阅，组件中没有 widget 缓存取数。

`query_project` 新增 `well_facies_models` 和 `well_facies_inputs`：前者声明 Read +
ExternalTransfer，返回端点身份及真实模型契约/缺项；后者只读 SDK 元数据、稳定曲线/道 ID
与当前固定层段引用，不读曲线数值。元数据预算超限整次拒绝，不伪装续页或完整覆盖。
SDK `listDataPage` 使用真正的 SQL keyset producer；指定井从 `(well_id, object_id)` 索引 seek，
每页最多 512 条及一条探针。分层道计数由事务触发器维护，不逐页扫描区间。
旧只读库缺索引时返回 DataPageIndexUnavailable；可写打开一次迁移，不全表回退。

冻结输入存于宿主创建的私有目录；父 prepare 操作冻结完整段清单，各段使用稳定子请求键。
父为真实 prepared、computeCount=0 的输入快照，工具使用 `inputOperationId` 和
`executionScope=batch_summary`，各子段另带真实 operationId，不把快照伪记为远端成功聚合。
取消父快照只停止尚未发送的段；已受理的原 job 仍允许 GET 核对和收取完成事实，之后不再 POST。
W5 协调器先持久化 Send 意图才允许 HTTP POST，收到原 job ID 立即持久化；
未知提交永不自动重发。恢复只查询原 job，未发送的新段须重新核对同一冻结模型契约。
已验证的完成响应先在 worker 保存 content-addressed 结果缓存，再写精确完成证明及同一
操作工件。因此无 job ID 的同步成功也可跨进程恢复，不靠模型或当前曲线重算。
聊天取消/工程切换后，已获知的完成事实和工件仍由原恢复 owner 收尾。

同请求键的完整回放先校验文档/聊天作用域、Read 和原意图摘要，无需当前模型 provider、
端点或写权限。回放仅返回持久执行回执与引用，明确 `resultAvailability=reference_unverified`、
`coverage.subject=durable_execution_receipts`、`resultBytesVerified=false`；
实际读取通过 operation_read 在 worker 校验字节/sha。已删除的结果不冒充可读数值，也不触发 POST。
这些作用域、输入/子操作指针、结果可用性和远端结局也纳入 W1 的有界元数据；即使缓存满或
进入 768 字节硬兜底，也不把 batch_summary 误记为输入快照执行成功。

## 验证记录

SDK 最终候选 `8c0a329`（基于 PR19 的 `39f8100`，经 `573db55`、`ebab4c1`）：独立生产 CMake
编译，`data_page_contract`、`well_query_contract`、`well_occurrence_contract`、
`stratigraphic_framework_tests` **4/4 通过**，0 失败，最终 8.88 秒。
其中生产 SQL builder 的 EXPLAIN QUERY PLAN 在一万条其他井曲线和一万条分层道之后
查询末尾小井，确认指定井+ID seek、无其他井扫描/相关 COUNT；同时覆盖真实值、重开、
移井、级联删除、区间移动计数、回滚、缺索引只读拒绝与可写重建。
真实 SQLite 首行成功、后续 next() 整数溢出的反例还验证了整页错误收尾，不能发布部分成功页。

```bash
cmake --build /home/kevin/build-paleo-sdk-well-occurrences --target \
  data_page_tests well_query_tests well_occurrence_tests stratigraphic_framework_tests -j1
ctest --test-dir /home/kevin/build-paleo-sdk-well-occurrences -R \
  '^(data_page_contract|well_query_contract|well_occurrence_contract|stratigraphic_framework_tests)$' \
  --output-on-failure -j1
```

源码门禁在 fetch/rebase origin/master 后，用显式
`QGIS_PREFIX=/home/kevin/paleo-agent-deps/prefix/usr` 启用 transitive 四指标，**34/34 通过**。
无基线增长；22 条实际 lupdate context/source 的中文与英文译文已补，TS missing=0。
日志：`/home/kevin/paleo-sdk-data-page-step-{build,tests}.log`、
`/home/kevin/paleo-well-gates-final.log` 和 worktree `local/well-source-gates.json`。

Paleo 五套已真实增量编译并 **5/5 CTest 通过**，8.33 秒；总计 **72 业务/82 含 fixture**，
均为 0 失败、0 跳过。新传输用例内部四个组合按一个业务用例统计，不把断言次数算成用例。

| 套件 | 业务/数据行 | 含 init/cleanup |
|---|---:|---:|
| tst_chatresults | 14 | 16 |
| tst_wellref | 8 | 10 |
| tst_wellfaciesinputplan | 22 | 24 |
| tst_wellfacies_remote | 21 | 23 |
| tst_wellfaciescontract | 7 | 9 |

```bash
cmake --build /home/kevin/build-paleo-agent-well --target tst_chatresults tst_wellref \
  tst_wellfaciesinputplan tst_wellfacies_remote tst_wellfaciescontract -j1
ctest --test-dir /home/kevin/build-paleo-agent-well -R \
  '^(tst_chatresults|tst_wellref|tst_wellfaciesinputplan|tst_wellfacies_remote|tst_wellfaciescontract)$' \
  --output-on-failure -j1
```

日志 `/home/kevin/paleo-well-five-{build,tests}-final.log` 与 build `Testing/qtest-first-run/`。
回归发现并修复 job ID 双重 URL 编码，且 HTTP 夹具明确区分 `/predict` 提交与
`/predictions/<id>` 轮询，不再把 GET 当成 POST 计数。输入预算反例使用真实 4097 条曲线/
129 条道和页间取消，超预算没有请求 body。

根集成树已编译真实生产应用，实际 `tst_wellfaciesagent` 为 **20 含 fixture / 18 业务，
0 失败、0 跳过**，默认及无 ORT 配置均通过。真实 owner 用例覆盖生产装配、本地 HTTP、
未知提交拒绝重发、固定输入恢复，以及进程在
完成证明写入后、工件尚未附加时硬退出的恢复，并检验父取消只挡新 POST、仍收原 job 事实。
同时覆盖入队后端点改变时完整回执仍可只读回放；整批累计矩阵见
[W6 验证记录](agent-providers-20261114.md)。独立 well 的全 UI 依赖构建按团队资源安排
暂停，应用与 owner 验收使用根集成树的实际产品链接。

本批使用合成 SDK 数据和 loopback HTTP；没有向真实远端传送工区数据。
未进行本机 Windows 全应用构建或全应用视觉验收。真实端点缺少 feature-unit 等契约时，
仍如实不可用；本补丁不扩大远端协议的去重或查询承诺。
