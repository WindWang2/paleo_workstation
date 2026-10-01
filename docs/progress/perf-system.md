# goal/perf-systematize — 性能工程体系化（2026-10-01）

方向：把性能从「单点优化」升级为「可持续工程体系」——启动延迟、UI 线程
阻塞清零、内存预算、构建期优化实验、门框架构升级。分支
`goal/perf-systematize-20261001`（基线 master 1b1e5be）。

## Oracle 对账

| # | Oracle | 状态 | 证据 |
|---|---|---|---|
| 1 | 启动分段工具可跑 + 基线入档 + 比率门测试注册绿 | ✅ | `tools/measure_startup.sh`；`docs/perf/baselines/startup_ratios.json`（9 轮实测）；tst_startup_trace 9/9 |
| 2 | UI 阻塞发现清单全处置 + ≥3 真实修复带测试 | ✅ | 3 修复（F1/F2/F3）+2 处带理由豁免族 + tst_ui_blocking 6/6；另修出 streaming 丢键 bug |
| 3 | 墙钟断言比率化/豁免入档 + 比例入档 | ✅ | `docs/perf/ASSERTIONS.md`：48 处全处置（比率 13% + 先例比率 23% + 豁免 64%） |
| 4 | LTO/PGO 实验报告入档（采用/否决+证据） | ✅ | `docs/perf/BUILD_OPT.md`：O3 否决（catalog_open +35% 一致回归）；LTO 采纳为 `PALEO_ENABLE_LTO`（默认 OFF）；PGO 两阶段见报告 §4 |
| 5 | 966MB 内存预算门（RSS 上限+无泄漏）全绿 | ✅ | tst_mem_budget：RSS 151MiB ≤ 门 483MiB（0.5×体）；6 轮循环净增 12KiB ≤ 门 32MiB |
| 6 | ctest 全绿 + layering --strict + ledger + push + PR | ✅ | ctest 133/134（唯一失败 tst_panels 为禁区已知并行闪红，串行绿 134/134，见遗留节）；layering --strict ✅；ui_invariants --strict ✅ |

## 簇1 启动仪表

- `src/services/startuptrace.*`：分段桩。process→main 读 /proc/self/stat
  starttime（动态链接+重定位墙钟——LTO/库布局实验的观测面）；各阶段
  mark；`PALEO_STARTUP_TRACE` 落盘 JSON（env 未设=零开销旁路）。
- 打点：main_entry→pre_qt_ready→qgis_app_ready→services_ready→
  theme_ready→main_window_ready→window_shown→first_paint（首帧=事件
  循环内首个主窗 Paint 事件过滤器）。
- 本机实测（9 轮中位，**有并行编译负载**，总时长 3.9-9.8s 波动下比率稳）：

| 段 | 中位 ms | 份额（门基线） |
|---|---|---|
| process→main（loader） | 1390 | 0.581（exec→首屏最大单段！） |
| qgis init | 113.5 | 0.088 |
| 服务装配 | 74.9 | 0.067 |
| 主窗构建 | 483.8 | —— |
| show→首帧 | 379.5 | 0.40 |

- 门：tst_startup_trace 四道份额比率门（基线 ×2.5 容差；**3 次起进程取
  每道门比率中位再判**——单轮冷启在共享机上 qgis 份额极值实测 2.04×基线，
  全量 ctest -j4 并行下单轮口径曾闪红，中位口径免疫单轮异常）。判别力
  证明用**相对劣化口径**（负载免疫）：同测健康进程 + 注入 1500ms 进 qgis
  段的进程，注劣化轮份额必须 ≥3× 健康轮（绝对门在重载下会被肥分母稀释
  ——实测注劣化份额可低至 0.18；相对口径不受分母影响）。
- 启动画像结论：loader 0.56 份额 + 主窗构建/首绘 ~0.86s——优化下手面是
  动态链接布局（预加载/库裁剪）与首帧渲染分工，不在 codegen（见 BUILD_OPT
  §3：O2/O3/LTO 启动份额一致）。

## 簇2 UI 线程阻塞清扫

静态扫描面：`grep readAll/\.open(/waitForFinished/exec()` in src/ui +
重 IO 门面同步调用（lasAt/wellCompositeAt/wellTopsAt/geoJson*）。
发现清单与处置：

| # | 位置 | 体量风险 | 处置 |
|---|---|---|---|
| F1 | datapreviewtabs well_log 页签同步 `lasAt` 整份解析 | 59MB 冷 ~400ms | **修复**：两段式（lasHeaderAt 秒铺骨架 + requestLas 池内装填，双消费方单次装齐；失败换装失败面） |
| F2 | 辅助 XML 预览同步 `loadComprehensiveXml` | MB 级曲线 XML | **修复**：`loadComprehensiveXmlAsync`（世代号防陈旧；comprehensiveXmlLoaded 信号） |
| F3 | entitypanel GeoJSON **双重 DOM 解析**（bounds+readAll 各一遍） | 大相图 2×300ms | **修复**：`geoJsonSummaryAt` 流式门面（无 DOM 两遍增量扫描）+ 任务池异步 + 会话缓存 |
| — | io/streaming GeoJsonScanner | —— | **顺手修真 bug**：新 properties 对象首键被上一要素遗留 `m_lastPropSep=false` 误判成值字符串而丢键；键长 <24 截断改全长（等价性测试对 DOM 口径钉死） |
| E1 | datalist loadUndoVault / patterncatalog / wellcompositestore sidecar | KB 级小边车，会话一次 | 豁免：体量与频次均无感知面 |
| E2 | layertree/layerprops/identify/profile 的 QML 样式与 CSV 导出 | 用户对话框动作点小文件 | 豁免：交互即期反馈语义 |
| E3 | correlationpanel 同步 lasAt 降级路径 | 仅无任务服务（测试） | 豁免：B1 已异步化，此为故意降级 |
| E4 | wellTopsAt/timeDepthAt/wellHeadsAt（分层 CSV 族） | 井级 CSV 数 MB 内、数十 ms 级 | 豁免（本轮）：中等体量；两段式模式已就绪，移交后续波次按需逐个迁 |

- 动态验证（tst_ui_blocking）：三修复各一测试——页构建/受理耗时 <0.5×
  同文件整份解析（比率门，回退同步必红）+ 等待期事件循环 20ms 分片 ≥2 轮
  （解析占 UI 线程则第一片吞全程）+ 数据完整到达断言。
- 发现的基建事实：T1 的 `requestLas`/`lasHeaderAt` 与 io/streaming 的
  `geoJson*Streaming` 此前均为**零消费方**——基建已建好但视图从未迁移
  （本轮全部接线）。

## 簇3 墙钟断言现代化

全仓 48 处处置入档 `docs/perf/ASSERTIONS.md`（比例：比率化 13% / 先例
比率 23% / 豁免 64%）。本波改写 4 文件 6 断言（薄余量 ~3× 者）：

- tst_cache_las：warm/mem ≤0.5×cold（旧 cold<50/warm<5）
- tst_perf_las：parseDoc <0.5×legacy（实测 0.028）
- tst_perf_segyindex：命中 <0.42×冷建（ratios.json 基线×1.2）
- tst_perf_catalog：10k/1k 打开 ≤25（实测 8.8）

每处保留 sanity 上限（2000/500ms 级，拦挂死不判回归）。豁免条目逐条
附实测锚+余量倍数或语义类别（60fps 帧预算/取消延迟契约等）。

## 簇4 构建优化实验

详见 `docs/perf/BUILD_OPT.md`。要点：**首轮顺序对比被并行负载污染**
（O2 基线虚高 3.2×，险些得出「O3 启动 -69%」的错误结论）——交错三轮
重测后：O3 否决（catalog_open_10k +35% 两轮一致回归）；LTO 采纳为
`PALEO_ENABLE_LTO=ON` 默认 OFF（las 冷解析 -40.7%/segy 命中 -45.2%/
catalog 灌库查询 -18~25%，无一致回归，全量链接 ~2.3×）；启动份额三
构建一致（codegen 不动 loader 段）。

## 簇5 内存预算

tst_mem_budget（966MiB 真工区，env 门控）：
- RSS 上限 0.5×体字节=483MiB（实测 151MiB/0.16×——直读后端不整载；
  「整读内存」≥1.0×、「双缓存」≥2× 必红）。
- 泄漏嗅探：6 轮 open→2 切片→释放净增 ≤max(32MiB, 0.5%×体)（实测
  12KiB；每轮漏一份切片 ≈41MiB 必红）。
- 采样纪律：malloc_trim(0) 后读 VmRSS（分配器保留堆还 OS 再量——降噪
  不掩真漏）。

## 簇6 基线档案

- docs/perf/BASELINE.md 增 §7（本波：启动分段/内存/断言比例/构建实验
  指针 + 真工区复测锚）。
- baselines/startup_ratios.json 新增（工具实测产出，禁手调）。

## 遗留移交

- E4（wellTops 族两段式迁移）按需逐个做——模式与 F1 同。
- tst_panels 并行闪红（模态拍序族）非本轮范围，未动：终验 ctest -j4 中
  `dataops_d1_batchAddTagViaInputDialog` 闪红（输入对话框交互拍序；与
  F1-F3 改动无涉——串行复跑通过，全量套其余 133 项全绿）。与 BASELINE.md
  §B5 既往记录同族，移交专项。
- 启动优化下手面：动态链接布局（loader 0.56 份额）——preload/库裁剪
  实验，未在本轮范围。
