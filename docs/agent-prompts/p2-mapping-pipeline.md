你在 paleo_workstation 项目实现「编图链路」：以 D61 为目标层位的 厚度→约束插值→相多边形→残差验证→三视图联动→PDF 导出 全链路，再加 8 层位 chip 切换和保存版本/发布/Published 状态机（对应 docs/PROJECT_AREA_PLAN.md 阶段 C + E）。

【执行方式】
- 单线程串行实现。**禁止**主动启用任何 workflow / teamwork / swarm / 并行子代理编排（不 spawn_subagent、不开 worker）。所有代码你自己写。
- 按 goal-loop 协议执行（若宿主有 /goal-loop 或 /goal skill 则调用之）：
  - 开始前声明一次：目标一句话、完成条件 Oracle（=下方【验收】全部通过）、迭代上限 40 轮
  - 在工作区根维护 `.goal-loop-ledger.md` 账本；每轮只做一处聚焦改动，亲自跑验证命令，记账 `第N轮 | 改动 | 验证结果 | 通过/未通过 | 下一步`
  - 验收未满足禁止宣告完成；卡住时换方法而不是退出；完成后关键验证连跑两遍
- TDD：每个交付物先写 QTest 失败测试（红），再实现到通过（绿）。测试文件用 `add_paleo_test()` 注册。
- 用 /qa、/review 类只读 skill 做自查可以；不要用它们衍生别的执行模式。

【环境】
- 仓库 /home/kevin/projects/paleo_workstation，C++20，Qt 6.11.2，QGIS 4.2.2 系统安装（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui，moc=/usr/lib/qt6/moc）。系统 QGIS 已装，勿 vendor。
- 构建：`ninja -C build`；测试：`cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure`。
- 权威契约：`docs/PROJECT_AREA_PLAN.md`（§3 契约 / §5 阶段 C、E / §7）；架构边界 `docs/PALEO_QGIS_PLAN.md`（§41.7 保存/发布语义、§1223 版本提交边界）；视觉 `DESIGN.md`；不做清单 `TODOS.md`。
- 先跑 gstack 自检：`_GS=""; for _D in "${GSTACK_ROOT:-}" "$HOME/.claude/skills/gstack" "$HOME/.grok/skills/gstack"; do [ -z "$_GS" ] && [ -n "$_D" ] && [ -d "$_D/bin" ] && _GS="$_D"; done; [ -n "$_GS" ] && echo GSTACK_OK || echo GSTACK_MISSING`
- 验收数据源：`/home/kevin/projects/paleo_project/data/project_area`。**禁止**把 966 MB SEG-Y 和 8 个层位 .dat 提交进仓库；夹具切片放 `testdata/`。

【第一步：建 worktree】
```bash
cd /home/kevin/projects/paleo_workstation
git worktree add ../pw-mapping-pipeline -b wave/mapping-pipeline
cd ../pw-mapping-pipeline   # 之后所有工作在此 worktree 内进行
```
本包预期在 `wave/data-foundation` 合并之后 rebase 到 master；若并行开发，CMakeLists.txt 冲突时保留双方新增的源文件行。

【与数据底座包的接缝】
另一并行包（wave/data-foundation）产出：catalog.json（实体/资产/关联/版本）、井的 surface_x/y+coordinate_status、井分层 tops、时深 TD 表、D61 的 411×641 DERIVED 时间栅格（登记进 LayerManifest）、SEG-Y 测线解码。
- 读侧门面契约已钉在 **`src/services/projectdata.h`**（主工作区已有；若开工时还未提交进 master，从主仓原样拷入你的 worktree，不得改 API）：`wells()`、`topsFor(wellId)`、`tdTableFor(wellId)`、`timeAtTvd()`、`horizonRasterDecl()` + WellRecord/WellTop/TdSample 结构 + catalog.json 字段契约注释。**不要改这个头文件**——两包共同编译它，接口漂移=合并冲突。你的工作是写 `src/services/projectdata.cpp` 的真实现。
- 开发期若对方未合并：用合成 catalog.json + 夹具栅格做 fixture 实现把测试跑绿；合并后用真实现替换，接口不变。
- 若对方已合并：直接接其 catalog 读取代码，不要自己另写一套实体解析。

【任务背景】
- 已有可复用：`ConstraintWorkflow::runConstraintIDW`（约束 IDW，凸包裁剪，已支持 CONSTRAINTS 图层与坐标变换）；`CompositionWorkflow::deriveFaciesPolygons`（`paleo:paleo_facies_polygonize`）；`fuseFactors`；`ValidationWorkflow::validate`（现查重复层位名/缺源/忙层冲突）；`ValidatePage::locateRequested` 信号（现只接 zoomToLayer）；`QgsLayout` 导出已通（src/ui/layout/）；`SelectionContext::activeHorizon` 切换机制已在。
- 现状缺口：没有按井 tops 算厚度；validate() 没有「井上时间 vs 栅格」残差检查；点开问题只做地图缩放，没有连井/地震联动；层位 chip 只切 activeHorizon 未限 8 个界面集合；没有版本/发布状态机。

【交付物】

1. **读侧门面实现** `src/services/projectdata.cpp`：实现 `projectdata.h` 的 `ProjectData` 接口（契约与 catalog.json 字段在该头文件注释里，逐字遵守）；fixture 实现（合成 catalog.json + 小栅格）放测试侧供本包自测；真实现接 catalog.json + LayerManifest。若 worktree 里还没有该头文件（master 未带上），从主仓原样拷入，不得改 API。

2. **阶段 C — 只编 D61**
   - 结构面 = D61 派生时间栅格图层。
   - 单因素：逐井取 tops 里 D61 与 D62 的 TVD 算厚度（缺 D62 分层的井跳过并计数），在测网网格上跑 `runConstraintIDW`（cellSize 取测网像元尺度，凸包裁剪沿用）。
   - 相多边形：以厚度栅格为输入调 `deriveFaciesPolygons`（ComposePage 已有入口），可选叠加已有融合结果；本阶段不做相序规则融合（plan §6）。
   - ComposePage / 工作流接线：串起「选 D61 → 算厚度 → IDW → 转相面」按钮链路，状态文案走 statusLabel。

3. **验证：时间残差 → 三视图联动**
   - 扩 `ValidationWorkflow`（或新增检查器）：每口有 D61 分层的井，用其 TD 表把分层 TVD 插成 TIME(ms)，与 D61 栅格在井位处的采样值求差；超阈值（默认参数可配，默认半样点间隔≈1ms 起评）记一条 `ValidationIssue`，带 wellId、井位坐标、目标测线（inline）、目标时间。
   - `ValidatePage::locateRequested` 扩为三视图联动：地图 `zoomToLayer` 到该井已有；新增连井面板滚到该井该分层、地震面板滚到该井对应测线和该时间。两个面板如需新滚动 API（如 `scrollToWellTop(wellId, horizon)`、`gotoLine(inline, timeMs)`），加在各自面板上并在注释标明为本链路新增。
   - 无 TD 表的井：问题记「无时深表」，不造假时间。

4. **布局导出 PDF**：一张含井位 + 相多边形的 D61 图，走现有 layout 导出管线；测试断言产物文件存在且非空。

5. **阶段 E — 8 层位 chip + 版本状态机**
   - 编图 chip 只列 {C3,C6,D53,D61,D62,D63,D71,D72}；切换沿用 `activeHorizon` + 按层位懒加载；井分层里其余名字只出现在连井面板。
   - 版本状态机（§41.7 + §1223 的最小语义，细节你自己在提交里写明决策）：
     - 「保存版本」= 编辑会话 commit → 版本号递增 + provenance 记录；commit 后 undo 栈清空，undo 不跨版本边界。
     - 「发布」= 导出 result/ 快照（gpkg + 布局产物），进入 `Published`。
     - Published 快照只读；继续编辑产生下一版本，不回写已发布快照。
     - 触发顺序按 plan：发布入口在 D61 的 PDF 能导出之后再暴露。
   - 状态机在 `PaleoProjectStore`/meta sqlite 落表（版本号、provenance、published 快照路径），schema 迁移走最简单的前向兼容（新版本字段可空）。

【验收 Oracle】（全部满足才算完成）
- `ninja -C build` 零错误；`QT_QPA_PLATFORM=offscreen ctest --output-on-failure` 全绿（含新增测试）。
- 新测试至少覆盖：tops→厚度提取（缺 D62 跳过计数）、IDW 厚度栅格在测网网格尺寸与凸包裁剪、厚度栅格→相多边形链路产出 `facies.D61` 声明、残差计算（TD 插值 vs 栅格采样，超阈成问题）、无 TD 井记「无时深表」、locateRequested 触发三视图调用（两个面板的新 API 用 spy/状态断言）、8 chip 集合与非集合 tops 不进 chip、保存版本后 undo 清空且版本号递增、发布后快照只读且 result/ 产物存在。
- 手工核对描述：一口井走完「问题→三视图」；D61 PDF 导出成功。

【纪律】
- 不碰：`src/io/`（数据底座包区域）、`src/metadata/` catalog 相关新增、`pagepanels.cpp` 里 DataPage 区域（另一包在动）——你为 ValidatePage/ComposePage 做的增量尽量集中、各自成块。
- CMakeLists.txt：新增源文件/测试集中放在一个连续块里。
- 不做：砂地比、距井距离、TIN、等值线、屏障 IDW、相序规则融合、多 realization、相界地质类型、地震体渲染/任意测线/三维（plan §6 + TODOS）。
- 完成后在 worktree 分支 `git add -A && git commit`，提交信息遵循仓库惯例。
- 合并：`cd /home/kevin/projects/paleo_workstation && git merge wave/mapping-pipeline`（建议在 wave/data-foundation 合并后进行；有冲突先 rebase 再合），合并后主仓 `ninja -C build && QT_QPA_PLATFORM=offscreen ctest` 连跑两遍全绿才算交付。
- 最终报告：测试通过数、改动文件清单、读侧门面的接口定义（供核对是否同 catalog 契约一致）、给集成者的注意事项。
