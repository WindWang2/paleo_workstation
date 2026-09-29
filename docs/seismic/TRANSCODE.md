# 转码链路（TRANSCODE）

> wave/seismic-chain-deep · Phase 1 交付（D1.1–D1.10）
> 前置阅读：[ARCHITECTURE.md](ARCHITECTURE.md)（管线全景）、[BASELINE.md](BASELINE.md)（实测）

## 1. 双通道总览

| 通道 | 产物 | 后端语义 | LOD | 典型耗时（220MB） |
|---|---|---|---|---|
| `.sf3c` | `<sgy>.sf3c.meta` + `.sf3.sNNN` 分片 | Auto 自动升级（随机访问） | 无 | 2.3s（raw）/ zstd 更慢但省盘 |
| `.sf3p` | `<sgy>.sf3p` + `.lN.sf3p` 兄弟层 | 显式（`Backend::Paged` 或显式路径） | L0+L1+L2(+L3) | 11.3s（三阶段） |

两通道可并存。UI 入口在数据页地震资产的时间切片工具条。

## 2. 分阶段进度与 ETA（D1.1）

引擎回调（`TranscodeProgress.phase`：`scanning|transcoding|finalizing`；
`PagedPipelineProgress.phase`：`l0-transcode|l1-build|l2-build|finalizing`）
在服务层经 `TranscodePhaseTracker` 加权聚合成**单调全局百分比**：

- sf3c 权重：scanning 4% / transcoding 92% / finalizing 4%
- sf3p 权重：L0 1.0 / L1 1/16 / L2 1/64 / L3 1/256 / finalizing 0.01

`PaleoTask::reportBytes` 吃到全局分子分母后，任务页的进度条与字节速率 ETA
（「约 Ns」）直接可用；阶段切换不再归零回退（`pagedAggregatedProgressMonotonic`
断言 regressions==0）。

## 3. 断点续跑（D1.2/D1.8）

- **探测**：`SeismicTaskService::probeWorkspace(sgy)` / `probePagedWorkspace(sf3p)`
  头级轻量读（不碰 chunk 表/shard），三态：未开始 / 已完成 / 可续跑
  （`chunksDone/chunksTotal`）；meta 不可读时带原因（版本/损坏）。
- **UI**：半成品 → 按钮变「继续转码」+ 确认框显示进度（「已完成 N/M 块」）；
  旧版/损坏 meta → 「重建工作区」话术；取消后再点同入口即续跑。
- **无假完成态（D1.8）**：vendor 补丁 P8——`Dataset::Open(Auto)` 对伴生
  `.sf3c.meta` 做 `ProbeWorkspaceMeta` 完整性判定，半成品保持直读后端并
  `FellBackToDirect()` 如实标记（`probeStatesLifecycle` 断言）。
- **续跑契约**：完成位图持久化（两通道）；`.sf3p` 未完成只有 `.partial`
  不发布；`.sf3c` 取消后 meta 在而分片不全——Auto 不当成品（见上）。

## 4. 并发分片写入（D1.3）

vendor 补丁 P6：`TranscodeOptions.writerThreads`（1..4，服务层恒 4）。
codec=zstd 时拓扑为：产者 → N 编码线程（BoundedQueue 容量 8）→ 单写线程
（容量 4）——压缩 CPU 摊到池上，写序与可续跑布局不变；raw 自动退单线程。

**关队语义**（并发正确性关键）：产者只关 encodeQueue；**最后一个编码器**
负责在排空后关 writeQueue——否则在途编码块被已关闭的写队列静默丢弃
（曾致 chunksWritten=0 的真 bug，`parallelEncodeMatchesSingleThread` 锁定：
并行产物与单线程逐采样一致）。

## 5. 质量报告（D1.4/D1.5/D1.10）

`SeismicTranscodeReport`（`startWorkspaceTranscodeDetailed` /
`startPagedTranscodeDetailed` 的 `onReport` 回调）：

| 字段 | 语义 |
|---|---|
| tracesRead / tracesTotal | 成功读取道数 / ROI 格点数 |
| coverage() | (written+skipped)/chunksTotal |
| missingTraces / damagedTraces | 源缺席 / 源读取失败（NaN 填充不炸整体，D1.5） |
| damagedSample | 前 32 个坏道 "inline/xline" |
| valueMin / valueMax | NaN 感知值域 |
| droppedRatio() | (missing+damaged)/tracesTotal |
| lodLevels | sf3p 实际构建层级（自适应结果） |

UI：完成弹窗一行摘要 + 按钮 tooltip 常驻；坏道>0 弹警告列出样例。

**结构化日志（D1.10）**：每行 `PALEO-SEISMIC-TRANSCODE {JSON}`，事件
`start|finished|cancelled|failed|rejected`，字段含质量全表——测试断言
JSON 可解析（`serviceReportAndLogs`）。

## 6. meta 版本化与迁移（D1.6）

- `.sf3c`：formatVersion=3，读取端版本不符即拒；探测报告版本号 →
  重建入口（引擎 probe 失败自动删 meta+分片重建，`corruptMetaDetectedAndRebuilt`）。
- `.sf3p`：version=7（legacy 6 可读）；`algorithmVersion`/`buildGeneration`
  独立字段。
- 迁移策略：不做静默就地迁移（数据安全优先）——旧版文件触发「重建」话术，
  引擎删除重建。真迁移工具留待版本真升级时（TODOS 登记）。

## 7. 同输出互斥（D1.7）

`activeTranscodeOutputs_`（主线程串行访问）：同输出路径在途转码时
`start*Detailed` 同步拒绝（`concurrent-transcode` 日志事件 + 中文错误），
终态释放。防止两个任务交错写同一分片集。

## 8. 金字塔层数自适应（D1.9）

`planPagedLodLevels(volumeBytes)`（二进制头轻量估计，失真只影响层数）：

| 体量 | 层级 |
|---|---|
| < 64 MiB | 不建 LOD |
| < 2 GiB | L1 |
| < 16 GiB | L1+L2 |
| ≥ 16 GiB | L1+L2+L3（16×16×1，从 L0 直建） |

计划外历史兄弟层自动清理；**旧 API `startPagedTranscode` 保持恒 L1+L2
语义**（`startPagedTranscodeImpl` 的 lodMode 1/2 分叉），Detailed 走自适应。

## 9. 复现

```bash
QT_QPA_PLATFORM=offscreen ./build/tst_seismic_transcode   # 13 用例
QT_QPA_PLATFORM=offscreen ./build/tst_seismic_baseline    # 转码耗时实测
```
