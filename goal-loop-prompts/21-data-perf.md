# Goal-Loop 方向 21：数据路径性能攻坚（索引缓存修复 / LAS 读面 / 冷启动 / catalog 热路径）

## 背景（实测事实，勿再勘察）

- **SEG-Y 索引缓存发布失败（可复现 bug）**：真工区 200P_seismic.sgy 启动时每次报——
  `[index] strategy=sequential failed: rename failed while publishing a scan checkpoint` →
  `[index] cache not published: Cache verification after writing failed` →
  扫描本身成功（263k 道 ~146ms）但 checkpoint 落盘校验挂 → **每次重启全量重扫**。
  checkpoint 路径 `~/.cache/paleo_workstation/index-cache/scan-checkpoints/200P_seismic.sgy.ckpt`。
  嫌疑面：原子 rename 跨文件系统 / 目录权限 / `.running` 旗标与校验器的写序竞争。
- **LAS 解析逐行读**：`src/io/lasparser.cpp` 逐行 sscanf 式解析——大单文件 LAS（几 MB+）解析数百 ms；`parseRange`/`parseHeader` 分列读已是先例（#108 用）。mmap/缓冲读 + 列向量化是明面收益。
- **catalog 热路径有基线**：`tst_catalog` 的 `thousandEntityQueryBudget` 实测基线（json=2297KB open=346ms 等断言值在测）——#107 迁 sqlite 后**基线数字没重测**，性能验收面可能虚标。
- **冷启动未知**：应用启动到主窗可用的全链路没 profile——QGIS 初始化 / 目录扫描 / 索引 / 面板构建各占多少没人量过。
- **既有性能纪律**：`testprobe.h`（perf::measure 门）、`docs/PERF.md`、`docs/progress/perf-systematize.md`（#4 轮的比率门 + ctest PERF 分类先例）；SEGY 索引已有 sequential/traceindex 双策略（io-perf 段）。
- **方向 20（JobRunner）可能动 `workflows.cpp`**——本方向不碰工作流编排面，只碰 `src/io`/`src/catalog`/`src/services` 读路径 + 启动序列。
- 纪律：每 `src/` 文件三层标记；`add_paleo_test`；`check_layering --strict` 绿；性能断言走比率门 + 绝对上限双轨（机器无关）；**先 profile 后优化**——不许猜测热点。

## 目标形态

一件 bug 修复 + 三条性能线，全是数据路径：

1. **索引缓存发布修复**（最优先，是可复现 bug）：定位 `rename failed while publishing a scan checkpoint` 根因（`.running` 旗标/checkpoint 校验器/原子写序逐一排除），修掉让缓存真正落盘——**修完冷启动 SEG-Y 索引应走缓存命中而非全量重扫**（断言验证）。
2. **LAS 读面加速**：`lasparser` 数据段读路径升级——缓冲行读→分块/mmap 二选一（写明取舍）；`parseRange` 列级读同步受益；ASCII 解析热路径优化（不引 fast_float 也可，先 benchmark 再决定）；`welllogset::readCurve` 多文件并发读（读 IO 可并行）。
3. **catalog 热路径重测 + 优化**：`thousandEntityQueryBudget` 基线在 sqlite 落地后重测更新；catalog open / 查询 / `linksForEntity`/`wellCurveIndex` 走索引后的热路径 profile + 热点修。
4. **冷启动 profile**：启动到主窗可用分段计时（QGIS init / 目录 open / 索引 / 面板）落文档；找得出大头就修，找不出如实记录分布。

## Oracle 验收（全部须实测通过并记账本）

1. **缓存修复**：真工区启动——首次扫后 checkpoint 落盘断言（`.ckpt` 文件在）；二次启动 `[index]` 日志走缓存命中路径（扫描耗时 < 首扫 20%）；`tst_segy` 索引面测试绿。
2. **LAS 读**：`tst_lasperf` 新探针——合成大 LAS（1M 行）解析时间比率门（新 vs 旧 ≥1.5×）；解析正确性回归（`tst_lasparser` 原断言全绿）；多文件并发读断言（2 文件并行 < 串行）。
3. **catalog 热路径**：`thousandEntityQueryBudget` 基线数字更新为 sqlite 实测值；open/查询时间 ≤ 旧 JSON 基线断言（sqlite 不得比 JSON 慢——如慢要记录根因）。
4. **冷启动**：分段计时文档落 `docs/progress/data-perf.md`；若能优化（如延迟加载某段）实测收益数字。
5. **正确性不回归**：所有 io/catalog/services 既有测试原样绿（性能优化不改语义）。
6. ledger 全账 + `docs/progress/data-perf.md`（缓存修复根因、LAS 读面口径、catalog 基线对比表、冷启动分段、递延：SEG-Y mmap 体读/属性体 GPU/并行索引）。

## 勘察指引

- `src/io/segyreader.*` + `src/services/seismicindex*`（索引缓存写路径、`scan-checkpoints`、`.running` 旗标、双策略）
- `src/io/lasparser.cpp`（逐行解析面、`parseRange`/`parseHeader` 分列先例）
- `tests/tst_catalog.cpp`（`thousandEntityQueryBudget` 基线断言值）、`src/catalog/datacatalog.cpp`（open/查询热路径、`linksForEntity`）
- `src/services/welllogset.cpp`（`readCurve` 多文件读面——可并发点）
- `src/app/main.cpp`（启动序列各段——profile 插桩点）、`docs/PERF.md` + `docs/progress/perf-systematize.md`（比率门先例）
- `tests/tst_segy.cpp`（索引测试面）、`tests/fixtures/`（合成 LAS 夹具先例 `makeSyntheticLas`）

## 禁区

- **先 profile 后优化**——不许没测量就改「热路径」；每个优化附前后对比数字。
- 不改索引文件格式（SEG-Y index bin 格式已定）；不动 catalog.sqlite schema（#107 已合，只能改读法不能改存法）。
- 不碰 `src/workflow`/`src/ui`（方向 20 在飞）；不碰 `src/algorithms`（方向 18 在飞）。
- LAS mmap 若跨平台（Windows 无 mmap）做不动，退回分块缓冲读并如实记账——**不硬做平台特定优化**。
- 性能断言走比率门/绝对上限，**不许写死绝对毫秒阈值**（CI 机器浮动会翻）。
- 不为提速牺牲正确性——解析结果必须逐字节等价（断言）。

## 迭代协议

- **轮0**：勘察定案——缓存发布失败的根因定位（复现 + 逐嫌疑面排除）、LAS 解析 profile（哪段热）、catalog sqlite 基线重测、冷启动分段计时原型；四个方向的实测数字进 ledger。
- **轮1**：索引缓存修复 + `tst_segy` 断言面扩。
- **轮2**：LAS 读面升级 + `tst_lasperf` 探针。
- **轮3**：catalog 热路径（基线更新 + 若有热点修）。
- **轮4**：冷启动 profile 收口 + 可优化段修。
- **轮5**：全量回归 + progress 收口。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/dataperf -b goal/data-perf-<date> origin/master`
2. 每轮原子提交，ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**：`git diff origin/master...HEAD` 全量自审；性能断言前后对比数字齐；无调试残留；层标记齐；`check_layering --strict` 绿；vendor 前缀全量构建零新警告；ctest 全绿；Oracle 每条有命令+输出证据。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
