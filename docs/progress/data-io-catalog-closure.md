# WP2 — Catalog / IO / Cache / Import Closure（数据底座收敛）

> 分支 `goal/data-io-catalog-closure-20260930-215316`（base master `ac882cf`，
> worktree ../pw-data-io-catalog-closure-20260930-215316）。
> 唯一目标：在现有存储模型（catalog.json 权威源）内把 mutator 写路径从超线性
> 收敛到线性，并收口导入/缓存/完整性面上仍可复现的真实缺口。
> 不做：catalog.sqlite 主存储、新数据格式、DataPage UI 重写、新地质功能。

## Phase 0 对账（BASE_SHA = ac882cfcfb93f606fdcc6c8813b71380759f86bd）

### Open / merged PR

- Open PR：0。
- 最近 merged 中与本 lane 相关：**#60**（io-perf-cache P4：查询面邻接索引
  O(1)、BatchSave、缓存体系、备份轮转——本包的写路径工作是它的直接续章）、
  **#64**（deepen-perf B4：发现并记档 mutator 超线性 10k 6.2s → 100k 949.7s，
  TODOS P3「catalog mutator 写路径超线性 profiling」即本包主目标）。

### Issue 真伪对账（2026-09-28 审计批，逐条在 BASE_SHA 复现）

| Issue | 审计主张 | master 现状 | 处置 |
|---|---|---|---|
| #26 | ProjectDirLock 应用层未取锁 | `appcontext.cpp:190-192` 打开工程即 tryLock，失败降级只读 | already-fixed-on-master，不动 |
| #40 | .bak 先删后拷 / ::rename 绕过宽字符替换 / LO profile 畸形 | save() 已走 bakTmp+paleoReplaceFile；`::rename` 在 src/io、src/workflow 零残留；LO profile 已 pid 隔离 + QUrl::fromLocalFile | already-fixed-on-master，不动 |
| #41 | .running 旗标多实例互删 | `crashreport.cpp:330` 已按 pid 分文件 `.running-<pid>` | already-fixed-on-master，不动 |
| #42.1 | SegyReader readByIndexList 静默丢道 | readInline/readCrossline 已有 "failed to decode N of M" 错误（半修）；readByIndexList 本体仍无留痕 | **本包收口**：丢道计数 + qWarning + 截断回归测试 |
| #42.2 | MapVersionStore horizon 未过段校验 | `mapversionstore.cpp:382` 已 isSafePathSegment 拒绝 | already-fixed-on-master，不动 |

### 远程分支重叠矩阵

- `wave/*` 与 `codex/prediction-facies-editing`、`codex/sections-time-depth`：
  全部 behind-only（ahead=0）——历史分支，无动作。
- `codex/simplify-duplication`：ahead 1（fc64457）。diff 集中在 metadata/workflow/
  ui/linkage（WP4/WP3 领域）；与 catalog/io/services 的重叠≈0（仅 tst_import.cpp
  +5 行、io/geojsonaffine.cpp 重构）。**不搬运**——该分支的通用 storage refactor
  属 WP4 领地，本 lane 不顺手带走。

### 基线（本机实测，构建口径见下）

（数字见 §性能 A/B——测量进行中填充）

## 根因定位（profile 证据，非猜测）

微基准（系统 Qt 6.11.2 -O2，`QVector<Ver>` 形状对齐 CatalogVersion；带/不带
COW 共享快照的 append）：

| n | 带快照 append（addVersion/addLink 现状） | 无快照 append | 比率 |
|---|---|---|---|
| 10k | 3,167 ms | 5 ms | 633× |
| 50k | 89,679 ms | 27 ms | 3,321× |
| 100k | 635,635 ms | 73 ms | 8,707× |

与 B4 登记的夹具全链数字（10k 6.2s / 100k 949.7s，RelWithDebInfo）量级吻合：
**主凶 = 六个 mutator 的 `previousVersions`/`previousLinks` 全表快照——每次
append 触发 QVector COW detach 的 O(N) 深拷贝（大块 mmap 分配 + 逐页缺页
零填充），N 次导入即 O(N²)。**

次级（静态确认，夹具分段计时复验）：

1. `markStaleDownstreamOf` 每次全表建 id→row QHash（m_idx.versionRow 已有
   同能力，纯冗余）——supersede 携带链的 addVersion 隐藏 O(N)/调用。
2. `nextEntityId(prefix)` 线性扫全实体表——导入井/辅助实体逐个发号 O(N²)。
3. `DataCatalog::versionBySha256` 与 `CatalogReadSnapshot` 四查询面
   （versionBySha256/entityById/wellsMatchingName/linksForEntity）线性扫表
   ——buildIngestPlan 逐项调用，N 文件目录 plan 构建 O(N²)。

## 交付

### 1. catalog 写路径线性化（主目标，TODOS P3 关闭）

- **`src/catalog/datacatalog.cpp`**：addVersion/addLink/attachLink/
  setLinkUnresolved/setLinkPrimary/markDownstreamStale 六 mutator 的全表
  QVector 快照全部换精确 undo——新增行 removeLast、被改行原值逆序还原。
  save 失败回滚语义与旧实现逐项等价（tst_perf_catalog::
  mutatorRollbackUndoesExactly 钉死：含 supersede 下游 stale 标记复原、
  主关联降级复原、未决备注保全、revision 不变、undo 栈同条目多入栈的
  LIFO 唯一正确序）。
- **`markStaleDownstreamOf`**：去掉每次全表建 id→row 哈希，行号走
  m_idx.versionRow（O(1)）；新增 undo 栈出参。
- **`nextEntityId`**：前缀最大序号走索引（旧线性扫全实体表）。
- **`versionBySha256`**：sha 命中行集走索引（旧线性全表扫；「第一个匹配」
  =表序语义不变，命中后文件复核逐字保留）。
- **`src/catalog/catalogindex.{h,cpp}`**：+`m_rowsBySha`（小写归一，行集
  升序）与 `m_entitySeqByPrefix`（「最后一个 '-'」拆解，与旧扫描对非空
  前缀同判定；空前缀分歧如实注释——现有调用方不可达）；rebuild/
  versionsMutated 同步维护；verifyAgainst 新增两表对账。
- **`src/io/ingestplan.{h,cpp}`**：CatalogReadSnapshot::fromCatalog 一次
  O(N) 建四张命中索引（sha→行集、实体 id→行[首行优先]、规范化井名→id
  [空规范化不入表——review P2 修复]、实体 id→链接行集）；plan 构建期四
  查询面去 O(N²)。live-vs-snapshot 等价测试（tst_ingestplan）保持绿。

### 2. 导入链/IO 完整性收口

- **#42.1 收口**（`src/io/segyreader.cpp`）：readByIndexList 对 decode
  失败计数并在非取消时 qWarning 留痕（「dropped N of M traces」）——
  traces() 等调用方拿部分结果时日志有账；readInline/readCrossline 既有
  "failed to decode N of M" 错误面不变。新增
  tst_segy::postOpenTruncationDroppedTracesSurface（open 成功后截断文件，
  断言错误文案 + qWarning 必发）。
- 导入链架构复核结论：BatchSave 单批次落盘、scan/hash/copy 全阶段协作
  取消、IngestPlan 三段式幂等、逐文件重试状态机、ProjectDirLock 单写降级
  ——均已在 master 就位（B2/IngestPlan/#26），本轮无需改动；快照查询面
  的 O(N²) 是本轮唯一真实规模缺口（已修，见上）。

### 3. 明确否决的方案（评估记录）

- **.bak 硬链接轮转**：O(1) 备份但 .bak 与主文件共享 inode，主文件原地
  写坏会同步毁掉备份——违背「上一代可回退」契约，保持整文件拷贝。
- **catalog.sqlite**：ADR 0056 触发条件未达（查询面 O(1) 保持），递延不变。

### 4. 测试与基线

- tst_perf_catalog +2（回滚逐项、序号/sha 语义含重开）；tst_perf_regress
  +1（build 比率门）；tst_segy +1（截断丢道）。ratios.json 新增
  `catalog_build_5k_vs_1k_max: 8.0`。夹具分段计时（PALEO_CATALOG_PROFILE）。
- BASELINE.md §B7、TODOS.md mutator 条目关闭、本文档 = 记录三件套。

## 性能 A/B

口径：同机（Arch/40 核，共享负载——三 lane 并行编译）、同 build type、同夹具
（tst_catalog_scale + PALEO_CATALOG_SCALE）。Debug 全量对比 + RelWithDebInfo 对比。

### Debug（build/，-O0+asserts）

| 规模 | BASE `ac882cf` | HEAD（本包） | 倍率 |
|---|---|---|---|
| 10k 灌库（4×N mutator + N 受管文件 + 1 次 save） | 38,642 ms | **1,184 ms** | **32.6×** |
| 100k 灌库 | **>3,600,000 ms**（QtTest 60 分钟超时中断，未能完成） | **12,737 ms** | **>283×** |
| 100k 打开 | （未达） | 4,089 ms | — |
| 100k 查询面（1000× entityById / linksForEntity / countsByType） | （未达） | 2 / 3 / 1 ms | O(1) 保持 |

10k→100k 数据 10× 时 HEAD 灌库耗时倍率 **10.75×**（≤15× 目标达成）。
分段（HEAD，100k）：entity 508 / asset 317 / version 3,106 / files 1,666 /
link 703 / flush 4,759 ms——全部线性；flush（一次全量 JSON 序列化 + .bak
拷贝）是剩余最大单项，属全量快照语义的必要成本，非重复扫描。
峰值 RSS（HEAD 100k 全程）：≈1,005 MB（单份四表 + flush 期 JSON 树；无
「复制 N 份 catalog」——修复后每 mutator 的瞬时全表深拷贝本身也消失了）。

### RelWithDebInfo（build-rel/ 与 BASE 独立 worktree build-rel/，-O2，仓库 perf 口径）

同机同 build type 对照（BASE 用 pw-wp2-base worktree @ `ac882cf` 定向构建）：

| 项 | BASE `ac882cf` | HEAD（本包） | 倍率 |
|---|---|---|---|
| 10k 灌库 | 17,121 ms | **1,140 ms** | **15.0×** |
| 100k 灌库 | **3,518,351 ms**（58.6 分钟） | **9,283 ms** | **379×** |
| 10k→100k 耗时倍率（10× 数据） | 205.5×（超线性实锤） | **8.1×**（亚线性） | 目标 ≤15× ✅ |
| 100k 打开 | 2,135 ms | 2,485 ms | 同量级（两侧各自带并行负载，JSON 全量解析主导） |
| 1000× entityById / linksForEntity / countsByType | 1 / 1 / 0 ms | 1 / 1 / 0 ms | O(1) 保持 ✅ |

对照 prompt 生成时的记录数字（另一台机）：100k 949.7s → 9.28s ≈ **102×**。
四项硬门全部达成；输出内容/revision/currentVersion/provenance 语义由
tst_catalog 37 用例 + 新增 rollback/seq/sha 用例钉死兼容。

### 计时门分诊（本机 Debug 全量第一轮 10 红 → 逐项归因）

| 测试 | 归因 | 证据 |
|---|---|---|
| tst_onnx / tst_onnxworkflow | 环境伪红（本机 QGIS 走 ~/wp2-prefix，测试属性 APPEND 的 LD_LIBRARY_PATH 覆盖后找不到 libqgis_core） | 单跑（含 QGIS lib 路径）OK；已加 CMAKE_BUILD_RPATH 根治 |
| tst_cache_las / tst_perf_las / tst_perf_segyindex / tst_perf_regress(segy 比率) | 本机 Debug(-O0) 口径既有现象，非本包回归 | **BASE Debug 同箱同红**（四项逐一对照）；HEAD RelWithDebInfo（预算校准口径，docs/perf 注明 -O2）四项全绿 |
| tst_previewmap_assets | 环境伪红（vendored GDAL 缺 GDAL_DRIVER_PATH） | 补 GDAL_DRIVER_PATH 后 29/29 绿 |
| tst_previewmap_canvas / tst_correlation_full | 本机负载敏感（共享机 sibling lane 并行编译，load≈11-17；correlation 3054-3347ms vs 3000 预算即 TODOS 在案的历史抖动签名 3041ms） | BASE Debug 侧同条件对照进行中 |

### 机器无关回归门（新）

`docs/perf/baselines/ratios.json` 新增 `catalog_build_5k_vs_1k_max: 8.0`
（门 = 9.6）——修复后实测 5.22–5.37（纯线性=5.0）；二次态 ~25 必红。断言
在 tst_perf_regress::catalogBuildScalingWithinGate，与既有查询门同一容差口径。

## 不做什么

- 不引入 catalog.sqlite（ADR 0056 触发条件未达——查询面 O(1) 保持）。
- 不把 .bak 轮转改成硬链接：评估后否决——.bak 与主文件共享 inode 时，主文件
  被原地写坏会同步毁掉备份，违背「上一代可回退」契约（腐败恢复演练正是原地
  截断主文件）。整文件拷贝保留，磁盘带宽换恢复语义。
- 不动 query 面（open/查询预算不回退，既有 tst_perf_catalog 门继续看护）。
- 不做 wellsMatchingName 的 **live catalog** 井名索引（真实工区 W≪N，非真实
  规模缺口；夹具 100k 井也不是该函数的消费路径）。导入 **worker 快照**侧的
  四个查询面（含井名）已索引化——那是 buildIngestPlan 逐项调用的真实 O(N²)。
