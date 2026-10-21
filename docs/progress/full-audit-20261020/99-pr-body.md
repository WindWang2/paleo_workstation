# refactor: comprehensive review-fix-review-fix quality closure (round 4)

## Summary

全仓深度审计第四轮（full-audit-20261020）：5 个只读审查 agent 覆盖上轮移交缺口
（correlation/seismic3d/layout 大面、singlefactor 全 17 文件、faultsurface 主体）
+ PR #579 新码全量 + 上轮登记未修 9 项复核；24 项 finding 修复（P2×4 / P3×15 /
P3L×5），双独立复审（R2 挑翻 2 条 MUST_FIX + 5 条 SHOULD_FIX；R3 终审抓 1 条
提交树断裂）全部闭合。

## Baseline

- BASE_SHA `ee8518df`（= origin/master，未漂移）；Linux x86_64（Arch 6.18.55-cachyos-lts）
- Ninja + RelWithDebInfo + QGIS_PREFIX=主仓 vendored 4.2.2（只读复用）
- 环境事件四起（Arrow 25/26 ABI 断链、ld 偶发段错误、ar 写入期偶发损坏、
  AppDir 库遮蔽 prefix）——全部 ENVIRONMENT_FAILURE，处置与证据见
  docs/progress/full-audit-20261020/06-baseline-quality.md；**未触碰共享 prefix**

## Review Coverage

src 12 模块 + tests/tools/cmake/ci：上轮移交未核查面全部补齐逐行
（B 域 singlefactor/faultsurface、D 域 correlation/seismic3d/layout/wellsection、
#579 14 文件）；E 域构建/测试/闸门全量对账；未核查面如实声明（02-coverage.md）。
上轮登记未修 9 项全部复核收口（证实/证伪/机制修正）。

## Findings

修复池 24 项 + RECORD-ONLY 若干；排除全部 4 个开放 issue（#556/#558/#559/#561）
与 PR #580 的 51 文件/16 已修项（EXCLUDED_ACTIVE_WORK——#580 分支已修的 G1 簇
闸门红在本分支仍显红，合并 #580 后自愈）。

## Fix 1（10 commit）

- `fix(io)` A-10：ziparchive ZIP64 减法判界（crafted 包负下标越界读）+ stored 等长校验
- `fix(ui)` D-05/D-07/D-10：seismic3d 换体弃置面板缓存+世代守卫；无体按钮弹回；§35 reason
- `fix(workflow)` C-10：#579 回收防线对称化（软删资产重导入拒绝 + declareMaps 过滤）
- `fix(services,domain)` A-14/B-03/C-14：sidecar 词表单一事实源 + bundle 族扫描
  （导入后生成的 .aux.xml/.qix/.qml 不再误报孤儿误删）+ copyDataset 自清
- `fix(io,services)` B-04/B-05：SATR 取消谓词归一 + 交会采样 4GiB 预算闸
- `fix(ui)` D-13/D-12/D-11：换勾等值距建议行归属守卫；参考面板空态；操作行 reason tooltip
- `fix(io,catalog)` A-13/A-11：catalog 负 ord 告警；segy 并行错位检测转顺序
- `fix(algorithms)` B-11/B-10：resolveParameters 2e4 前置闸；faultsurface 多链如实上报
- `fix(ci,tools)` E-10/E-12/E-13/E-11 部分：triage 三盲区+steps 精度+startup_failure；
  qgis_spread ctest 注册对齐；PALEO_PYTHON3 显式失败；flaky selftest 接闸
- `fix(workflow,qgis,ai)` C-11/C-15/D-11：surveyArea 度制守卫；QPointer 关停守卫

## Review 2 与 Fix 2（6 commit）

- **R2-MF-2**：seismic3dviewpanel 超 file-budget/ui-size 冻结值（1827/1003）→
  setVolume 拆出 volume TU（1775/1003 回位，冻结值未动）
- **R2-MF-1**：D-13 测试断言必红 + tooltip 残留 → 无建议路径复位默认 tooltip
- R2-SF-1/3/5：渲染器 ClearStackLayers；copyDataset 自清测试；triage 文案区分
- **R3 终审 MUST_FIX**：QDoubleSpinBox include 落在未提交工作树（HEAD 不编译）
  → 补提交 + 提交树重编译复跑

## Final Review

R3 独立终审（极端输入 5/5 推演、层归属、ODR、基线零违约、仓库卫生全部未推翻）：
初判 FAIL（include 断链）→ 闭合后 **PASS-WITH-NOTES**。遗留 NOTES 均登记
（SF-2 recycle schema 下沉、SF-4 大输入行为面、A-10 stored 理论洞、#559 墙钟家族）。

## Testing

- 提交树全量：**427/440（97%）**，13 红全部 master 既有/在册（10 项 G1
  簇=PR #580 已修未合、#533 transitive 挂起、tst_ui legendTitle 与 tst_assetpaging
  在册基线红）——**零本轮新增回归**（首轮 426/440 中的 tst_catalog 为 -j4 负载
  墙钟抖动，串行两连绿 + 提交树复跑未再现）
- 红绿双证 5 簇（A-10/B-04/B-05/D-13/C-10：master 版红→修复版绿，06 文档表格）
- 门禁：layering(--strict)/i18n/docs(不变部分)绿；transitive 52/50 持平
  （唯一红=#533 在册）；file-budget/ui-size/ui-tokens 的红全为 #580 未合的 G1 面
  （本分支冻结值零改动）
- 基线文件零改动；known-reds 仅收紧（lint 条目补 steps 声明）

## Performance

B-05 修复的量化面：VRT 巨网格采样 master 33.3s 无界分配 → 修复 7ms 拒绝
（红绿双证）；其余无性能声明项。

## Conflict Avoidance

- 未触碰 #556/#558/#559/#561 根因；未重复 #580 的 16 已修项（其修复面在
  master 仍带 bug，本分支不重修——同文件其他新问题照常处理并注明）
- 与 #580 的 rebase 交集：无共同修改文件冲突（#580 触碰 51 文件中本分支仅
  datalist_batch.cpp/paleomapimport 族语义相邻，文本面独立）

## Known Limitations

1. 共享 vendor prefix 的稳定性（Arrow 断链/重建）已三次干扰构建——建议 prefix
   重建加协调锁（BUILDING.md 纪律已写，执行面待加强）
2. tst_catalog sqliteOpenCostAttribution 裸墙钟断言在 -j4 负载下可抖（#559 家族）
3. E-11 跨 run flaky 台账聚合（artifact 回灌）未接——selftest 已接闸，机制归 #501
4. 上轮 X-01 的 C-12（worker 内 QgsLayer）维持可辩护口径未动
