# goal/data-perf — 数据路径性能攻坚（方向 21）

> 分支 `goal/data-perf-2026-10-03`（自 `origin/master` `ede43ce` 起，worktree `.worktrees/dataperf`）。
> 四条线：① SEG-Y 索引缓存发布 bug 修复 ② LAS 读面加速 ③ catalog 热路径重测与归因
> ④ 数据路径冷启动分段。本文是收口底账；逐轮命令与原始输出见
> `.goal-loop-ledger-dataperf.md`。
>
> **测量环境**：Windows 11 / MSVC 14.38 / Qt 6.8.0 / QGIS 4.2.0（vendored，
> `paleo-qgis-prefix`）/ RelWithDebInfo。所有绝对毫秒均为本机值，**不可跨机器
> 比较**；断言一律用比率门 + 绝对上限，不写死毫秒阈值。

## 交付一览

| 线 | 问题 | 处置 | 前后数字 | 测试 |
|---|---|---|---|---|
| ① 索引缓存 | checkpoint（纯可恢复性优化）发布失败被当致命错误 → 整卷扫描作废 → 缓存永不落盘 → **每次重启全量重扫** | 发布降级为 best-effort + 失败可见（`checkpointNote`/`checkpointFailures`）；tmp 名带 pid+序号；发布前预建目录；3 次退避重试；错误带 OS 码 | 冷扫 17.1ms → 命中缓存 2.8ms（ratio 0.162）；修复前**二次启动仍全量重扫** | `tst_dataperf_index` ×3 |
| ② LAS 读面 | ASCII 数据行的十进制→double 转换是唯一热点 | 逐位精确快速路径（≤15 位有效数字 + 指数∈[-22,22] ⇒ 一次 IEEE 运算正确舍入，否则回退原转换）+ 按首 64KB 行密度预估分桶 + 裸指针扫行 | 1M 行 `parseDoc` **478.5ms → 130.9ms（3.7×）**；比率门 0.789/0.743 → 0.282/0.205；4M token 与 `QByteArray::toDouble` 零位差 | `tst_lasperf` ×4 |
| ③ catalog | #107 落 sqlite 后基线数字仍是 JSON 时代推测值；「sqlite 稳态 open 比 JSON 慢约 1.6×」是猜测 | 基线重测为实测值 + 同进程内二分归因 | 稳态可写 open 45-49ms，其中 **23ms 全在写侧安全动作**；sqlite 只读口径 26ms **低于** JSON 时代 29ms | `tst_catalog`（`thousandEntityQueryBudget` + 新增 `sqliteOpenCostAttribution`） |
| ④ 冷启动 | 不知道用户打开工程时数据面钱花在哪 | 新增 `tst_coldstart` 逐段计时 + 热路径对照 | 合计 198-214ms，其中 **catalog_open 135-163ms（68-77%）**、SEG-Y 索引 39-50ms、LAS ~10ms | `tst_coldstart` |

## ① 索引缓存发布修复

### 根因（不是「为什么 rename 失败」，是「失败之后发生了什么」）

`vendor/sbm/src/Data/Sgy/`（in-tree vendored）：扫描循环里 checkpoint 写失败
→ `failed = true` → `ScanSegySequentially` 返回 `false` → `SgyIndexService`
走 `!scanned||!complete||!index` 早退分支 → **后面的 `saveCache` 块根本不执行**
→ 缓存永不发布 → 每次重启全量重扫。

一次 rename 抖动（杀毒软件、索引器、跨卷移动）就能让一整卷已扫完的索引作废。
checkpoint 是**可恢复性优化**，它失败不该让扫描本体失败。

### 修复

- `writeCheckpoint` 失败不再置 `failed`，改为记录 `outResult.checkpointNote` +
  `stats.checkpointFailures`；调用点从 `if(!write){failed=true;break;}` 改为直接调用。
- 失败原因不再被吞：`SgyIndexBuildOutcome::checkpointNote` 一路传到
  `SgyIndexService` 日志（`| checkpoint disabled: <reason>`）。
- `SaveCheckpoint` 的 tmp 名加 pid + 单调序号（原来固定 `.tmp`，同名 SEG-Y 会互踩）。
- 发布前 `create_directories` 目标目录；`ReplaceFileAtomically` 3 次 5ms 退避重试，
  消息带 OS 细节（Windows `MoveFileExW error <n>` / POSIX `ec.message()`）。
- `SgyIndexCache::ReplaceFileAtomically` 同样加固；temp 回读校验区分「打不开 /
  中途读失败」与「字节不符」，各一次有界重试（Windows 对刚关闭的句柄重扫曾伪装成损坏）。

**禁区遵守**：SEG-Y 索引文件格式（`kCacheFormatVersion=3` / `kAlgorithmVersion=2`）未动。

### 证据

`tst_dataperf_index` 用平台无关手法强制 rename 失败（把 `.ckpt` 预置成**目录**）：
POSIX 是 `EISDIR/ENOTEMPTY`，Windows 是 `ERROR_ACCESS_DENIED`——与真工区日志同形态。

反证（把 vendor 修复回退、临时摘掉 note 断言后重跑）：

```
FAIL: scan was discarded by a checkpoint failure:
      MoveFileExW failed while publishing a scan checkpoint.
```

修复后：

```
PASS: checkpoint note = rename failed while publishing a scan checkpoint (MoveFileExW error 5).
      cold=17.1ms warm=2.8ms ratio=0.162
```

## ② LAS 读面加速

### 先测后改

`lasbench` 微基准（400 万 token）先证明「热点是转换引擎本身」并量化所有候选 API：

| 方案 | 400 万 token |
|---|---|
| `QByteArray::fromRawData` + `toDouble` | 168.3 ms |
| `QByteArrayView::toDouble` | 213.3 ms |
| `std::from_chars` | 208.4 ms |
| `strtod`（scratch 缓冲） | 262.7 ms |
| **精确快速路径 + 回退** | **38.6 ms**（回退触发 0/4000000） |

结论反直觉：Qt 自己的转换器已经是最快的现成 API。真正的空间在**避开通用转换路径**。

### 快速路径的正确性论证

十进制串 → double，只在两个操作数都**精确**时，一次 IEEE 运算的结果才是
正确舍入的。快速路径仅在满足下列全部条件时启用，否则回退 `QByteArray::toDouble`：

- 有效数字 ≤ 15 位（双精度 15-17 位十进制才能唯一往返）
- 十进制指数 ∈ [-22, 22]（`DBL_MIN`/`DBL_MAX` 的十进制指数范围）
- 尾数可精确表示为 int64

**验证**：400 万 token + 25 个对抗样例（超长尾数、边界指数、`.`/`-`/前导零/溢出）
与 `QByteArray::toDouble` 逐位比对，**0 处差异**。

### 结构改动

- `parseAsciiRows` 改用 `std::vector<QVector<double>>` 按首 64KB 的行密度预估分桶容量；
  扫行走裸 `const char *lineBase = raw.constData()`（避开 `QByteArray::at()`），
  结束后一次性 move 回各列。
- `tokenToDouble` 两处调用点同步更新（`parseAsciiRows` 热循环 + `parseDepthRange`）。

### 前后数字（同会话严格 A/B：revert → rebuild → measure → re-apply）

```
before  parseDoc 478.5 / 481.7 ms
after   parseDoc 130.9 / 126.5 ms      → 3.7×
比率门（旧/新） 0.789 / 0.743 → 0.282 / 0.205
并发多文件  serial 881.3→643.2ms  parallel 245.2→191.2ms（ratio ≈0.28-0.30）
```

## ③ catalog 热路径：重测 + 归因

### 基线重测为 sqlite 实测值

合成 1000 井 / 2000 资产 / 3000 版本 / 4000 链接（json 2297KB → sqlite 1136KB）：

| 口径 | 实测 |
|---|---|
| `jsonParse`（5 次中位） | 13.4 ms |
| `open`（含 JSON→sqlite 迁移） | 490 ms |
| `reopen`（sqlite 稳态可写重开） | 45-49 ms |
| `jsonLegacyOpen`（#80 只读 + 只有 JSON） | 29 ms |
| `wells` / `match` / `shaMiss` / `links` / `current` | 均 <1 ms |
| `save` | 1-2 ms |

### 「sqlite 比 JSON 慢」的根因（同进程内二分，不靠减法猜）

用 `setLockedReadOnly(true)` 在**同一份 sqlite** 上打开——这条路径走
`readEpoch → integrity_check → loadTables`，跳过 `applyWritablePragmas` /
建表脚本 / `wal_checkpoint` / 备份轮转 / 二次 attach。差额即「每次可写 open
多付的那套动作」：

```
db=1136KB  open(migration)=488ms  reopen=49ms  roCatalogOpen=26ms  jsonLegacyOpen=29ms
writeOnlyExtras=23ms = [ schemaExec 2ms | integrity_check 8ms | readAllTables 8ms
                       | wal_checkpoint 1ms | fullFileCopy 1ms | 关连接-重attach-二次load ]
```

**结论**：sqlite 只读口径 26ms **低于** JSON 时代 29ms——读侧不比 JSON 慢。
稳态可写 open 的 45-49ms 里，那 23ms 全在 #107 之后每次可写 open 必跑的
写侧安全动作，与「sqlite 慢」无关。

### 断言口径（禁区：不写死毫秒）

- 整表读完 `<` 稳态 open
- 只读裸开 `×4 <` 可写 open（归因成立的前提）
- 只读 catalog open `<` 可写 open
- 只读 catalog open `≤` JSON 时代 `×2 + 20ms`（结构性 sanity，不是性能门）
- 稳态 reopen `≤` 迁移 open；`≤` JSON 时代 `×3 + 20ms`
- 迁移 open `<` 2000ms（挂死护栏）

## ④ 数据路径冷启动分段

### 为什么测的是数据面而不是 GUI 进程

GUI 冷启动（QGIS 初始化 / 面板 / 首帧）本机测不了，属**环境限制**：

- QGIS prefix 只有 4 个 dll、缺 `apps/` + `share/`，所有链 QGIS 的二进制
  （含 `paleo.exe`、`tst_boot`、`tst_runtime`）都是 `STATUS_DLL_NOT_FOUND`；
- 沙箱拦 `QProcess`（`tst_startup_trace` 的三个真实进程用例因此失败——
  在 `origin/master` 上 stash 全部改动后同样失败，非本方向引入）。

`StartupTrace` 的 7 段仪表（`main_entry → pre_qt_ready → qgis_app_ready →
services_ready → theme_ready → main_window_ready → window_shown → first_paint`）
保持不动，本方向不重做 GUI 编排。

### 数据面画像（真实工作区：1000 资产 catalog + 10000 道 SEG-Y + 5 万行 LAS）

| 段 | 冷（首触） | 热（对照） | 占比 |
|---|---|---|---|
| `catalog_open`（进程冷，1000 资产） | **135-163 ms** | 147-174 ms（换目录，200 资产） | **68-77%** |
| `segy_index`（10000 道 / 12MB） | 39-50 ms | **4.7-5.7 ms**（缓存命中） | 20-25% |
| `las_first_read`（5 万行 / 2MB） | 9.5-11.7 ms | 9.0-9.4 ms | ~5% |
| `las_header` | 1.7-1.8 ms | — | <1% |
| `segy_locate`（按 inline/xline 找道） | 0.001 ms | — | ~0% |
| **合计** | **198-214 ms** | 161-188 ms | |

**结论**：
1. `catalog_open` 是冷启动的绝对大头。轮③ 已把它的构成拆开——读侧不比 JSON
   慢，贵在每次可写 open 的写侧安全动作（integrity_check + 整库备份拷贝 +
   checkpoint + 建表脚本 + 二次 attach）。
2. SEG-Y 索引是第二大头，而轮① 的缓存修复已把它从 39-50ms 压到 4.7-5.7ms
   （省掉约 90%）。**这正是原来「每次重启全量重扫」的成本。**
3. LAS 读面现在只占 ~5%，轮② 的 3.7× 把它从约 40ms 压到约 10ms——修之前
   它本应是第一大段。

### 探针自身的两个坑（已在 `tests/tst_coldstart.cpp` 注释里留档）

1. **计时器**：`catalog_open` 含 `PRAGMA journal_mode=WAL`、
   `wal_checkpoint(TRUNCATE)` 这类要拿**排他锁**的操作。`QElapsedTimer` 在
   锁等待期间读数失真（本机测出 30s/60s 这类整数值，而真实值是 179ms）。
   全探针统一改用 `std::chrono::steady_clock`。
2. **同目录连开会量到锁等待**：`CatalogStore` 每次可写 open 都要 checkpoint +
   整库备份拷贝 + 关连接重连；同目录紧接着再开一次会等自己的锁
   （`busy_timeout` 5s/次），测到的是锁等待而非 open 本身。「热路径」对照
   因此换用另一个工程目录——这也更贴近「用户连开两个工程」的真实形态。

## 递延项（本方向只记录，未做）

| 项 | 依据 |
|---|---|
| **SEG-Y 体数据 mmap 读** | 冷启动画像里 `segy_locate` 已 0.001ms、`segy_index` 已被缓存压到 5ms；体数据读取不在启动关键路径上，改 mmap 收益不明、风险高（页缓存语义 + 跨平台回退） |
| **catalog open 写侧安全动作的削减**（`integrity_check` 8ms / 整库备份拷贝 1ms / 二次 attach） | 已定量但**不在本方向动**：这三项是崩溃安全与损坏恢复的机制（#79/#80 的契约面）。任何削减都要先回答「什么时候可以不做」，属独立设计决策，不是性能优化 |
| **属性体 GPU 化** | 与数据路径无关，属渲染方向 |
| **并行索引**（多线程扫卷） | 轮① 的缓存修复已让「重复扫」不再是常态（39ms → 5ms）；单次扫的并行化在 10k 道规模收益 <10ms，不值得引入并发复杂度 |
| **GUI 进程冷启动分段** | 本机环境不可测（见上）；`tst_startup_trace` 已有仪表与比率门，环境修好后可直接用 |
| **mutator 写路径疑似二次方** | `deepen-perf` B4 已记（10k 6.2s → 100k 949.7s）；属写路径，与本方向四条线（读面/缓存/冷启动）正交 |

## 已知既有失败（与本方向无关，`origin/master` 上同样失败）

在 `origin/master` 上 stash 全部改动、重建后复现确认：

- `tst_perf_las::errorClassificationEncoding`
- `tst_cache_las::secondOpenUnder5ms`
- `tst_welllogset_perf::wellCurveIndex_100x3_under500ms_and_linearRatio`
- `tst_perf_catalog` / `tst_wellcomposite_perf`（QGIS 链接，`STATUS_DLL_NOT_FOUND`）
- `tst_startup_trace` 的三个真实进程用例（沙箱拦 `QProcess`）
- 所有链 QGIS 的目标（QGIS prefix 不完整）
