# Goal Loop ledger — boundary-kinds（方向 39：相界地质语义类型）

- 日期：2026-10-04；分支 `goal/boundary-kinds-20261004`，自 `origin/master` 24daf8f 起；worktree `.worktrees/boundary-kinds`。
- vendor 接线：`vendor/superbuild/prefix`、`vendor/onnxruntime` 软链主仓；`vendor/prefix` 主仓不存在（无需）。
- Oracle 五条：三类着色 round-trip（含工程存取不回退）/ 编辑语义三场景 / 词面三处逐字一致 / 未知 kind 中性 / QA 按类型至少一条生效。每轮补命令与实测输出；未跑不标通过。

## 轮0 — 勘察与定案

### 实测落点（fault_cut 先例链）

- 词表：`src/workflow/boundarysemantics.h`——四类冻结（conformable/pinchout/facies_change/fault_cut），`activeKinds()` 仅 fault_cut；composepage 下拉与样式类目都从词表/硬编码取。
- 图面：`QgisStyleService::applyFaciesBoundaryStyle`（qgisstyleservice.cpp:496）——boundary_kind 分类渲染，仅 fault_cut（#D71414 1.0mm）+ 空值/其余中性桶（#5D6E80 0.26mm）；面填充统一低透明灰。无字段/非面层不接管。
- 编辑：composepage `faciesBoundaryKindCombo`（activeKinds 驱动）→ `faciesAttributesSaveRequested` → 壳 lambda（paleomainwindow_attach.cpp:2192）→ `CompositionWorkflow::saveFaciesAttributes`（FieldSpec 补建字段 + edit buffer 写 + kind 变更即时重刷渲染器）。错误经 statusLabel + QgsMessageLog 回显。
- QA：`faciesqa`（algorithms，纯 GEOS）五检测器 + 诊断计数；workflow 侧 `faciesmappingworkflow` 阶段4 从 CandidateRegion 装配 units（无 kind 面向——draft 单元不带 boundary_kind）。
- 方向31线型：`geopatterns.cpp` LineDef——facies_definite(#5D6E80 0.5 solid)/facies_inferred(0.4 dash)/facies_transitional(0.4 dot)；资源有 bnd_pinchout_line.svg、bnd_unconformity.svg 等。
- i18n：中文源串 tr() + `translations/paleo_zh_CN.ts`（lupdate 骨架，tools/update_translations.sh）。
- 测试先例：tst_composeworkflow（真实栈：AppContext+MainWindow，makeRaster→polygonize→saveFaciesAttributes）；tst_services2（内存层渲染器断言）；tst_faciesmapping_algorithms（QA 引擎构造单元测试）。

### 定案（语义定义——要素级 kind 的最小诚实面）

1. **图面**（复用方向31线型，不登记新图式）：
   - 整合接触 conformable → 实线 0.5mm #5D6E80（= facies_definite）；
   - 尖灭 pinchout → 虚线 0.5mm #5D6E80（虚线 = 端部不确定，沿 facies_inferred 惯例）；
   - 相变 facies_change → 点线 0.26mm #5D6E80（= facies_transitional）+ **渐变带**：额外符号层，宽度 data-defined 绑 `transition_width` 字段（无带/0/字段缺失 → line_style=no 不画）；
   - 断层切割不动；空值/表外值 → 中性桶（诚实面：不猜类）。
2. **编辑语义**（新 `workflow/boundaryeditrules`，纯语义无 Qt 挂件依赖）：
   - 整合接触赋类：跨边界邻接要素相代码已知且不同 → 拒绝（不切割两侧相）；相代码未知不参与拒绝（诚实中性）。
   - 渐变带：`transition_width` > 0 仅相变可携带；负值拒绝。
   - 开放端：尖灭允许环不闭合；其余三类拒绝；未分类不判（几何缺口归 QA UnclosedRing 管）。
   - 挂点：saveFaciesAttributes 赋类/带域前逐选中要素检查，拒绝则整单失败回原因（原子）。
   - UI：composepage 增「渐变带宽」spin，仅相变选中时启用（禁用带 reason tooltip，DESIGN.md §35）。
3. **QA 挂接**（只接不重写）：
   - FaciesMapUnit += boundaryKind/transitionWidth（默认空/0——draft 路径零行为变化）。
   - 新检测器与豁免：尖灭环不闭合豁免 UnclosedRing（开放端合法）；尖灭端点无落位（PinchoutTipDangling，容差可配 ≤0 关）；相变无渐变范围（TransitionBandMissing）；整合接触切两侧相（ConformableCutFacies，跨单元几何邻接判）。
   - 编图层消费入口：composepage「边界核查」→ 壳 → `CompositionWorkflow::runFaciesBoundaryQa`（从 facies.<h> 要素装配 units 跑同一引擎，报告有名有因回 statusLabel）。
4. **逐弧段（目标3）递延**：见轮次结论——boundary-graph 线层需弧段提取/共享弧归属/编辑回写三套新机制，且当前无弧段级编辑面（vertex tool 是要素级），无消费方；维持要素级，TODOS P2 条目补递延理由。

### 语义决策记录

- 「整合接触不切割两侧相」落地为：该要素与任一跨边界邻接要素的 facies_code 均相同（邻接=共享边线段，非仅相交）。两侧相不同时诚实拒绝并提示改用相变/断层切割。
- 「相变允许渐变带」落地为：transition_width（图层长度单位）随赋类提交，图面以数据定义宽度的半透明带渲染；带域仅相变携带（其余类型拒绝）。
- 「尖灭允许开放端」落地为：编辑规则接受不闭合环 + QA 豁免 UnclosedRing；端点应落位（贴邻接单元边界/约束线），悬空则 QA 报 PinchoutTipDangling。

## 轮1 — 全量实现 + 测试绿 + 自审修复

### 交付面（原子提交计划）

1. `feat(workflow)` 词表四类全激活（activeKinds→kinds + isKnownKind）+ `boundaryeditrules`（赋类/带域/环闭合三检查，纯语义）+ tst_boundaryeditrules（7 用例）。
2. `feat(qgis)` applyFaciesBoundaryStyle 四类目（整合实线 0.5/尖灭虚线 0.5/相变点线 0.26+渐变带层/断层红 1.0；带宽 data-defined 绑 transition_width，MapUnits；无带或字段缺失 → 线型 no 不画带）+ tst_services2 扩展（6 类目词面、三线型、带层 DD、表外/空值中性桶经 symbolForFeature 实测）。
3. `feat(workflow)` saveFaciesAttributes 门禁（boundaryFactsOf 装配：外环首尾重合/共享边邻接/相代码未知不判）+ transition_width 字段（double）+ CompositionWorkflow 构造挂 layerInstantiated → facies.* 重挂样式（instantiate 从源新建，编辑态渲染器不随实例存活——工程重开样式不回退的机制）+ tst_composeworkflow 门禁端到端（拒绝零副作用/尖灭过/带宽随非相变拒/相变+带过/换类型不锁死）。
4. `feat(ui)` composepage 渐变带宽 spin（仅相变启用 + reason tooltip）+「边界核查」按钮 → boundaryQaRequested；壳接线 → runFaciesBoundaryQa → statusLabel 条数+首条、全量进消息日志；UI 可用面测试。
5. `feat(algorithms)+feat(workflow)` faciesqa 三新检测器（PinchoutTipDangling 容差可配/TransitionBandMissing/ConformableCutFacies 共享边长度定位）+ 尖灭 UnclosedRing 豁免（draft 路径 boundaryKind 空 → 零行为变化）+ runFaciesBoundaryQa（要素→units 装配，多部件逐部件成单元 regionId 后缀 .pN；报告有名有因；井覆盖/孤岛/约束冲突归方向 27 draft QA 不冒充）+ 算法测试 3 用例 + QA 端到端（transition_band_missing → 手改 conformable → conformable_cuts_facies → 修复态零项）+ 工程往返测试（commit → write → open → 重实例化：数据与渲染器双存活）。
6. `fix(ui)` **附带修复（真实产品 bug）**：QgsSnappingConfig 镜像进工程时缺 project 指针——nativeSnappingConfig 是无 project 静态构造，QGIS 4.2 setSnappingConfig 不代挂；带主窗口的会话存工程后重开，readProject 在空 project 上调 mapLayer 段错误。两处镜像点（paleomainwindow/editingtoolbar）补 setProject。往返测试即崩溃复现器。
7. `chore(i18n)` lupdate 骨架全量刷新（704 新词条多为方向 24–38 未刷新积压；本方向新增约 20 条）。
8. `docs(todos)` P2 条目收口 + 逐弧段递延理由。

### 自审发现与处置（轮1）

- **[Medium] 换类型死锁**：旧门禁对「要素现值 transition_width>0 且本次赋非相变类型」拒绝——页面无清带入口，用户被锁死。修复：带域门禁只判本次提交的带宽提案（attrs 携带才判）；旧带宽数据惰性保留（非相变类目不渲染带、QA 不核查），不静默清零。补测试：facies_change+500 → 改 pinchout 成功、带宽数据保留。
- **[Low] 测试注释/计数**：SimpleFill 自绘描边（非独立线层）→ 层计数与注释修正（services2/composeworkflow）。
- **[Low] compositionworkflow namespace 后双空行**；services2 注释「三 层」错字。
- **[预存，非本方向] tst_services2::editingCommitFailureStillFrees** 在干净基线即红（commitEdit 失败路径 busy 标记未释放，QgisEditingService/PaleoProjectStore 面，与本方向零交集）——stash 验证证据见轮1命令记录。
- **[预存，非本方向] ui_tokens** 35 违规在干净基线即同数（35=35，staleExceptions 空，本方向零新增）。

### 验证命令与输出（轮1）

- `cmake --build build -j8 --target tst_boundaryeditrules tst_services2 tst_faciesmapping_algorithms tst_composeworkflow tst_mappingpages paleo`：0 error。
- `ctest --test-dir build -R '^(tst_boundaryeditrules|tst_faciesmapping_algorithms)$'`：2/2 Passed（0.61s/0.31s）。
- `./build/tst_services2 faciesBoundaryStyleFourKindsLineTypesAndBand faciesBoundaryStyleOnlyWithKindField`：4/4 PASS（含 0 FAIL 子项）。
- `ctest --test-dir build -R '^tst_composeworkflow$'`：Passed 276.70s（7 子测试含新增 4）。
- `ctest --test-dir build -R '^tst_mappingpages$'`：Passed。
- `ctest --test-dir build -R '^(layering|layering_strict|layering_selftest|i18n|i18n_selftest|ui_invariants|ui_invariants_strict|ui_invariants_selftest)$'`：全绿。
- 崩溃复现/修复验证：`QT_QPA_PLATFORM=offscreen ./build/tst_composeworkflow boundaryStyleSurvivesProjectRoundTrip` 修复前 SIGSEGV（QgsSnappingConfig::readProject → QgsProject::mapLayer 空 this，帧 #5–#10 openProject:167），补 setProject 后 PASS。
- 全量 `ctest -j8`：首轮因基线构建半途而废（管道吞退出码——262 Not Run）重跑全量构建后复跑（见轮2）。

## 轮2 — 全量绿 + review 轮2 + 收口

### 全量验证（worktree 全量构建 547/547 后）

- `cmake --build build -j8`：首轮 479/1015 处工具链偶发崩溃（`cc: Internal error (段错误)`，README 已知 ld/gcc 偶发——重试续传）后 547/547 成功。
- `ctest --test-dir build -j8`：**279 项中 274 过（98%），5 失败全部与本方向无关**：
  - `ui_tokens`（35 违规）：stash 基线同数 35，staleExceptions 空——预存，本方向零新增。
  - `tst_services2`：仅 `editingCommitFailureStillFrees` 子项，stash 基线同红（commitEdit 失败路径 busy 标记未释放，QgisEditingService 面，零交集）；本方向两个子测试 PASS。
  - `tst_layoutexport`/`tst_catalog`/`tst_startup_trace`：-j8 满载并发下失败，**单跑（带本方向改动）全 PASS**——负载抖动（README 有 -j2 偶发竞态同族先例）。
- 受影响面复跑：tst_boundaryeditrules / tst_faciesmapping_algorithms / tst_services2（新增面）/ tst_composeworkflow（7 子测试）/ tst_mappingpages / layering(_strict/_selftest) / i18n(_selftest) / ui_invariants(_strict/_selftest) 全绿。

### review 轮2（零 High/Medium 判定）

轮 1 修复项复核：
- [Medium→修] 换类型死锁：带域门禁改判本次提交的带宽提案；测试 `facies_change+500 → 改 pinchout 成功、带宽数据惰性保留` 过。
- [Low→修] 层计数注释（SimpleFill 自绘描边）、namespace 后双空行、services2 注释错字——全改。
- 全 diff 二遍扫：无调试残留/死代码；新文件头三行层标记齐；i18n 新串全 tr() 且 lupdate 收录；分层 strict 绿。

遗留 Low（记 PR body 不阻塞）：
- L1 runFaciesBoundaryQa 会实例化未装载的目标层（惰性装载语义，与应用既有路径一致）。
- L2 换类型后旧 transition_width 数据惰性保留（非相变类目不渲染带、QA 不核查；文档化的有意语义，不静默清零）。
- L3 上述 5 项预存/抖动红（证据在案）。

### Oracle 验收对照（证据所在）

1. **三类着色 round-trip**：tst_services2::faciesBoundaryStyleFourKindsLineTypesAndBand（四类目词面/线型/带层）+ tst_composeworkflow::boundaryStyleSurvivesProjectRoundTrip（commit→write→open→重实例化后 fault_cut 类目与要素数据双存活）。
2. **编辑语义三场景**：tst_boundaryeditrules（尖灭开放端过/整合切两侧拒/相变带可过、带宽随非相变拒）+ tst_composeworkflow::boundaryEditGateRejectsConformableCut（真实栈端到端：polygonize 两相带邻接，拒绝文案含冻结词面+两侧相码+要素定位，拒绝零副作用）。
3. **词面一致**：boundaryUiAffordances 断言下拉五词面与 boundarysemantics.h titleFor 逐字一致 + services2 断言渲染器类目标签同词面；属性值侧 data 断言稳定 id。
4. **未知 kind**：services2 经 symbolForFeature 实测 erosion/空值落中性桶（catch-all/空串类目）不 crash 不猜类；runFaciesBoundaryQa 对表外值零类型核查项。
5. **QA 项**：tst_faciesmapping_algorithms 三新检测器用例 + tst_composeworkflow::boundaryQaReportsByKind（transition_band_missing/conformable_cuts_facies 有名有因，修复态零项）。

### 附带交付

- fix(ui)：snapping config 缺 project 指针的工程重开段错误（真实产品 bug，往返测试即复现器；两处镜像点补 setProject）。
- 逐弧段递延（目标 3）：结论入 TODOS P2 与轮 0——boundary-graph 线层三套新机制 + 无弧段级编辑消费方。

### 收口

- 8 个原子提交（feat×5 / fix×1 / chore×1 / docs×1），分支 goal/boundary-kinds-20261004。
- push + gh pr create（不等 CI）。
