# refactor(02): 测井、井轨迹、连井剖面与现有交会分析闭环

**分支** `goal7/well-sections-crossplot-20261014` · **base** `origin/master` @ `b0520429` · **方向** 02（测井/井轨迹/连井剖面/交会）

## 范围与所有权

只对本方向已存在功能做迭代完善。写入面（全部在本方向白名单内，未触碰 PR #437 保护区与任何 Open Issue 指纹）：

| 文件 | 改动 |
|---|---|
| `src/algorithms/petrophys.cpp` | curveStats 方差消碳修复（Welford） |
| `src/workflow/wellsectionworkflow.{h,cpp}` | 剖面单位别名表统一 + 解释岩性/相解析跨请求缓存 |
| `src/ui/crossplot/crossplotpanel.cpp` | 通道选择跨重建保留 |
| `src/ui/wellcomposite/multiwellview.{h,cpp}` | CorrelationOverlay resize 几何跟随 |
| `src/ui/wellcomposite/wellcompositecanvas.cpp` | 自适应比例尺 resize 后读数刷新（死分支修复） |
| `src/ui/wellcomposite/paneltracks.cpp` | 会话曲线配置恢复（applySpecsIncrementally 注入） |
| `tests/tst_petrophys.cpp` 等 6 个测试 | 见下逐轮明细 |
| `docs/progress/parallel-7/02-*.md` | 本方向 ledger |

## 交付明细（六轮微循环，每轮缺陷→修复→测试）

1. **数值可信度**：`curveStats` one-pass 方差在 {1e9, 1e9+1, 1e9+2} 类数据上消碳
   （stddev=0，真值 1）→ Welford 累推。独立程序红绿对拍：OLD stddev=0 / NEW stddev=1。
2. **行为完整性**：连井剖面曲线装载只认 FT/F/M，METER/METERS/METRE/METRES/FOOT/FEET
   的 LAS 整文件静默丢弃 → 改走 `wellnumeric::depthScaleToMetres` 权威词表
   （空单位/未知词表仍拒绝，领域契约不变）。
3. **状态闭环（交会图）**：refreshSources 重建通道列表清空用户多选 → 同 id 恢复选中。
4. **UI 几何**：MultiWellView overlay 不随容器 resize（correlation 线裁剪）→ resizeEvent 跟随；
   自适应比例尺 resizeEvent 判据用错变量致死分支（读数陈旧）→ 判据修正 + 读数/图例同步。
5. **状态闭环（井道图）**：会话记忆恢复道宽/显隐但不恢复曲线集/量程（重开回退默认分组）
   → applySpecsIncrementally 注入 TrackConfig 曲线与量程覆盖。
6. **性能/告警噪音**：解释岩性/相 JSON 每请求重读盘 + 重复告警 → 按「解析路径+mtime」
   键控的跨请求缓存（GUI 线程单线程触达；失败同样缓存防告警风暴）。

## 验证（本机 Linux/Arch，复合 vendored deb 闭包栈，offscreen）

- 全量构建 `cmake --build build -j4`（全局 flock 互斥）**BUILD_EXIT=0**（2042 步）；
  review 修复后增量重建（164 步）与 stash 往返后重建均 exit 0。
- 六个微循环目标 ctest：**7/7 通过**（tst_petrophys 16/16、tst_crossplotpanel 7/7、
  tst_wellsection_workflow 24/24、tst_wellcomposite_multiwell 11/11、
  tst_wellcomposite_depth 20/20、tst_wellcomposite_framework 19/19）。
- 领域子集（井/剖面/井道/交会/对比/岩性 50 项）：**50/50 两遍全绿**（终码二遍）。
- 全套 384 项一遍：377 过 / 7 失败。7 项失败（singlefactor×5、tst_catalog、
  tst_algorithm_harness）**全部经净基线（stash 本 diff 后重建）复现同红**——
  确证为基线既有环境红项（CRS WKT 往返在 vendored QGIS/PROJ 下不等、
  GTiff 夹具写盘失败、ctest 沙箱时序），与本分支改动零因果；其中 tst_catalog
  直跑 44/0 全绿（失败仅在 ctest 沙箱内）。相关测试不触及本分支任何改动面。
- 红绿对拍（独立程序，旧算法 stddev=0 必红 / 新算法 stddev=1 绿）见 ledger 轮次 1。
- `python3 tools/check_layering.py --strict` 通过；`git diff --check` 干净。

## 独立 Review

S5 独立 reviewer（非实现者）九项审查（资源/线程/信号/缓存/回归/数值/测试/层/保护区）
全 PASS，**blocker=0**；建议 1/2/3/4/6/8 已采纳落地（读失败不缓存钉死、缓存上限 64、
Q_ASSERT 线程归属、#412 行号漂移登记、readCurveTvd 遗留分裂登记、文档补齐），
建议 5/7 判定无害保持最小 diff。全文见 ledger 轮次 7。

## 七方向冲突排除

- PR #437（80 文件）逐文件比对：本分支零交集（`wellpredictionreview.cpp` 虽匹配
  `*well*` 白名单模式但在保护区，未触碰）。
- Open Issues #352–#421 指纹表见 ledger；#412/#368/#389/#407/#415/#418/#387 全部避让。
- 与并行方向的构建通过 `/tmp/paleo_workstation_global_build_j4.lock` 串行（本轮实测与
  seismic-ai 方向构建排队共存）。

## 已知风险与限制

- 构建/测试需前置 vendored `LD_LIBRARY_PATH`（Arch 系统 Qt 6.12 与闭包 6.10.2 并存；
  复合栈复用 seismic-ai 方向同 lock 闭包——SHA256 同源，硬链只读共享）。
- snapshot.ubuntu.com 间歇 5xx 使全量闭包直拉过慢，改硬链复用并行方向已验证闭包
  （未修改共享目录任何文件）。
- 未处理事项与下一阶段建议见 ledger「留给 Issue Agent / 下一阶段」节。

## 未处理的 Issue 摘要

本分支不修复、不认领、不关闭任何 Open Issue；邻接发现（含 file:line 证据）已登记在
ledger 供 Issue Agent 复用，含 exportengine 分页漏带/SVG 比例尺（避让 #368 文件）、
剖面计划步 GUI 线程同步 IO（需方向07 任务 API 配合）等。
