# QGIS 编辑/编图/出图链深化（任务 B：goal/qgis-edit-compose-deepening）

日期：2026-10-13。基线：origin/master `8e963cd7`。worktree：
`../paleo_workstation-goal-qgis-deepening`（独立 build/，vendored
superbuild prefix 以 symlink 共享，未清理/未重编共享 prefix）。

范围：**既有**「图层管理—空间编辑—约束/单因素展示—综合编图—验证—导出」
链路的操作完整性深化。不新开业务模块、不造第二套 GIS 引擎/编辑器/composer、
不碰 PyQGIS。R0–R6 六向读码盘点（3 个只读探查代理 + 主读）后按证据选块；
每块「修前红 → 修后绿」对拍。

## 排除矩阵（开工前 gh 核实）

- **Open PR #432**（goal/ci-mappingpages-20261013）占用
  `tests/tst_mappingpages.cpp` —— 本任务零触碰该文件。
- **Open Issues #351–#421（全部 70 个）零处理**：另派 Agent 的对象。逐项
  核对清单类 issue 正文指纹（#411 attach_validate 布点工具泄漏 /
  #412 wellcomposite·seismic 边界 / #413 datapreview·constraintdraw /
  #415 workflow 八项 / #416 services 三项 / #419 src/qgis 五项 /
  #421 tests 七项 / #410 nativeattributetablehost 测试缺口）——本任务
  各改动点与其零重叠（#419 的 removeDeclarations 守卫缺口与本任务
  qgislayerservice 的 instantiateHorizon 失败上抛分属不同 hunk/缺陷）。
- **近期 merged PR 不重做**：#426（层位 chip/发布门/图签块/theme rename/
  窄面标签）、#430（单因素「先约束再提取再计算」主链）、#425（#351–#421
  补丁）、#424（资料图谱）、#427/#428/#429/#431。
- 文件所有权：写入集中在 `src/qgis/`、`src/ui/{layers,layout}/`、
  `attach_{constraint,compose}.cpp`；`attach_shell.cpp` 仅两处最小装配
  补丁（层位过滤器注入 + 画布浏览目标跟随，见 Block 5）；
  `src/workflow/mapexport.cpp` 为任务书预先批准的 B 所属编图导出编排。

## 交付块（每块红绿对拍见「验证记录」）

### Block 1（R1-D1）层位实例化失败不再无声消失

`QgisLayerService::instantiateHorizon`（qgislayerservice.cpp:274）原先以
`nullptr` error 逐层 instantiate——坏源声明层不进工程，图层树/画布/属性表
无迹可寻，层位切换后「无声消失」。现逐层收集原因，经新信号
`horizonInstantiationFailed(horizon, failures)` 上抛；
`LayerTreePanel` 接信号入全局 `ErrorHub`（warning 级通知卡，DESIGN.md
错误呈现口径；未安装 hub 的裸壳降级 qWarning）。全成功不发信号（零噪音）。

### Block 2（R1-D3 + 实证衍生缺陷）单因素上图/编图参考图的页主题对称

- **回写不对称**：预测页「显示结果」（workbench V3）拨完可见性会
  `captureCurrentAsTheme(page:*)`，而单因素互斥上图（attach_constraint
  V4）与编图参考图勾选（attach_compose V5）不回写——切页/换层位按旧
  `page:*` 主题回放，用户刚上的图被静默撤下。两处补同款回写。
- **组三态失明**（调试期实证）：V4/V5 用普通 `setItemVisibilityChecked`，
  互斥下勾把 04_SingleFactor/层位组三态拨成全关后，再勾子层不恢复祖先组
  ——图「上了」却看不见。显示路径改
  `setItemVisibilityCheckedParentRecursive`（V3/V8 既有口径），隐藏保持
  普通勾选（不强制显他层）。

### Block 3（R4-3）导出快照缺源层可核实

`PaleoLayoutExport::rebuildProject` 原先忽略 `readLayerXml` 返回值、
也不检查重建层有效性——worker 重建时数据源离线的层被静默跳过，导出
「成功」但产物少层。现逐层收集 warning（readLayerXml 失败 **或**
`!layer->isValid()`；memory 层灌回要素前天然 valid，不误报）：
`ExportOutcome` 新增 `warnings` 字段；设计器异步导出把 warning 并入
outcome（GUI 侧状态栏提示 + ErrorHub warning 通知卡）；四条后台导出
管线（设计器/层位图 PDF/按层位批量/地图册）的 worker 统一经
`paleo::workflow_detail::reportSnapshotWarnings` 入账（ErrorHub 任意线程
可调，未装 hub 的单测降级 stderr）。

### Block 4（R4-2）图件字体族收敛 vendored 单一事实源

`standardelements.figureTextFormat` 钉死系统族名 `"Noto Sans CJK SC"`
（standardelements.cpp:39），而 vendored 注册族是 `"Noto Sans SC"`
（paleotheme.cpp:43）——应用内 vendored 字体不被取用，PDF 文本落到
fontconfig 替换（`fc-match "Noto Sans SC"` 实测解析到 **KR** 变体）。
现进程内解析一次并缓存：vendored `Noto Sans SC` 在场优先，退化系统
`Noto Sans CJK SC`，再退化交给渲染端。DESIGN.md「vendor 打包保证跨平台
一致」口径落地。

### Block 5（R2-1）编辑工具条层位过滤器宿主注入（最小装配）

`PaleoEditingToolbar` 头文件契约（editingtoolbar.h:34-39）明示
「活动层位只可编辑该层位图层」由宿主注入 filter——全仓无注入（仅测试自
注入），跨层位实例化层（验证链/收编层）可在任意页签进入编辑。attach_shell
注入业务过滤：有声明层位的图层须 `horizon == activeHorizon()`；层位无关
共享层、用户自建层、服务缺席（裸壳）、**活动层位为空（工程刚开未点 chip）**
均不受限（review 修正：空上下文不设限，避免静默清空下拉）。配套两处装配：
- `SelectionContext::activeHorizonChanged` → `refreshFromProject`（层位
  切换不必然伴随图层增删——目标层位层已显式实例化时无 layersAdded/Removed
  信号，下拉会滞留旧层位可编辑集）；
- 树选中的被过滤矢量层仍是合法浏览目标：画布跟随树选择（原先只照顾栅格；
  过滤层选中时画布 currentLayer 滞留）。

### Block 6（R4-1）批量层位出图异步路径测试（test-only）

`horizonbatchexport.startAsync`（#349 交付物）零测试——补快乐路径
（worker 私有工程出图 → journal 经 owner 线程发布 → 结果回带真实文件路径
+ OUTPUT 登记）+ busy 重入拒绝 + 完成后 busy 释放 + 取消收尾。stale
catalog 回滚分支未覆盖（需与 worker 竞速，无法确定性构造）——如实记为
未覆盖。

## 验证记录

- **构建**：全量 `cmake --build build -j4` 0 error（含 paleo 与全部测试
  二进制；review 修复轮同样全量 0 error）。
- **红绿对拍**（临时回退生产修复、保留新测试，逐块确认转红）：
  - Block 1：禁用 emit → `instantiateHorizonSurfacesFailures` 红
    （failures.size() 0≠1）；恢复绿。
  - Block 2a：显示路径回退普通勾选 → `factorVisibilitySurvivesPageSwitch`
    首个 `node->isVisible()` 红（checked=true isVisible=false，层位组
    三态全关实证）；恢复绿。
  - Block 2b：移除回写 → 同测试终局断言红（「切页回放不得撤下」）；恢复绿。
  - Block 3：回退为零收集（原行为）→ `missingSnapshotLayerSurfacesWarnings`
    红（warnings.size() 0≠1）；恢复绿。
  - Block 4：回退钉死系统族名 → `figureFontFamilyResolvesToInstalledFamily`
    红（family≠"Noto Sans SC"）；恢复绿。
  - Block 5：移除 filter 注入 → `editingToolbarListsOnlyActiveHorizonLayers`
    红（非活动层位层进了下拉）；恢复绿。
  - Block 6：既有路径补覆盖，无红可对（路径行为未改）。
- **新增/扩展用例**：tst_layerservice +1、tst_layertreepanel +1、
  tst_shell_horizonstate +2、tst_layoutexport +2、tst_horizonbatchexport +1、
  tst_composeworkflow 1 处对齐（补 setActiveHorizon，见 Review）。
- **护栏**：`git diff --check` 0；`check_layering.py --strict` 通过；
  `check_ui_invariants.py --strict` violations=0；`check_l10n.py` 通过；
  `check_tidy.py` 改动 TU 全过。
- **关键测试双遍**（review 修复后两轮）：tst_composeworkflow 9/9 ×2、
  tst_shell_horizonstate 6/6 ×2、tst_layerservice 13/13 ×2、
  tst_layertreepanel 21/21 ×2、tst_layoutexport 16/16 ×2、
  tst_horizonbatchexport 6/6 ×2、tst_mapbook 18/18 ×2。

## 独立 Review（B5 角色，非作者）与修正

通读全 diff（signal 生命周期/线程纪律/undo/资源泄漏/i18n）。发现与处置：

- **P0（已修）**：tst_composeworkflow 补 setActiveHorizon 时缺
  `selectioncontext.h` include——编译失败且此前的「验证通过」实为 stash
  基线轮的陈旧二进制假象。补 include 后重建，9/9 双遍。
- **P1（已修）×3**：
  1. 空活动层位（工程刚开、未点 chip）时过滤器把全部层位层清出下拉——
     行为回退。改为 `activeHorizon()` 为空即不设限（没有可保护的层位
     上下文）。
  2. 警告通道只接了 4 条导出管线中的 2 条——`horizonbatchexport.cpp`、
     `mapbookqueue.cpp` 补齐（worker 收集 + 统一入账）。
  3. 缺层警告呈现两套口径（状态栏/消息日志 vs ErrorHub 通知卡）——统一：
     `workflowerrors_internal.h` 新增 `reportSnapshotWarnings`（worker 线程
     直报 ErrorHub，未装 hub 降级 stderr）；设计器 GUI 侧
     `reportExportOutcome` 同步 raise ErrorHub（warning 档，DESIGN.md 口径）。
- **P2（记录/处置）**：
  - undo 栈拒绝路径与 `isEditing()==false` 并存时树/画布/组合框三处分叉
    （可达性低：commit/rollback 清原生栈）——风险记录。
  - filter 裸捕 this 的生命周期靠隐式不变量（editTb 是 window 子对象）——
    已在注入处注释钉死「宿主须活得比工具条久」。
  - 层位切换 O(K²) 次 declaration SELECT（layersAdded 逐层触发刷新 × 过滤
    逐层查声明）——与既有 D6 热点同面，记为后续性能轮（合并刷新+批量取
    声明）。
  - 失败串中英混排（instantiate 底层英文 error 直达通知卡正文）——底层
    串既有，本轮不改，记录。
  - rebuildProject 工厂失败路径（typeOk/layer null 的 continue）仍静默
    丢层——记录（极边缘：XML 结构损坏）。
  - tst_horizonbatchexport 未钉 vendored 字体（跨机字体解析差异）——该
    测试最小链接集不含 paleo_ui，为 P2 级关切扩链接面不值得，记录放弃。
  - tst_layertreepanel 的 ErrorHub 装拆改为作用域守卫（QVERIFY 早退也
    还原全局指针）。
- **查过无问题**（reviewer 逐项实证）：tr() 带 %n 复数用法、qWarning 降级
  频率、parent-recursive 互斥正确性、findLayers 多节点防御、memoryBacked
  判定时机（vendored readLayerXml 先恢复 id）、字体 static 缓存线程安全
  （QFontDatabase 官方线程安全 + magic-static）、QgsMessageLog worker 调用、
  ExportOutcome.warnings 追加位不破坏既有聚合初始化、新测试驱动真实生产链。

## 全量 ctest（HEAD）vs master 基线（同 SHA `8e963cd7`）

- **HEAD 终轮**（review 修复后）：364 项（-LE perf）——362 通过 / 2 红：
  `tst_ui`（Subprocess aborted）、`tst_mappingpages`。
- **master 同 SHA 基线直跑归因**（stash 干净树重建二进制后单跑）：
  - `tst_mappingpages`：master 直跑 5 断言红（12+5/10）——PR #432 的修复
    对象，master 自带红，非本分支引入。
  - `tst_ui`：master 直跑同用例 `testSimultaneousWellsAndSurveyAreaVisibility`
    fatal error 红——master 自带红。
  - `tst_tileinference`：首轮全量在本分支红过一次；master 基线与本分支
    直跑/ctest 复跑均绿（并行负载抖动；本分支零涉及 src/ai）。终轮绿。
  - `tst_composeworkflow`：**本分支曾引入红**（编辑层位过滤器按契约拒绝
    非活动层位的自动编辑）——master 基线 9/9 对照定位；测试补 chipbar 同款
    双调（激活层位是生产必经步骤）后 9/9 双遍。


## 未做与边界

- R1 盘点的 D2（custom order 期复制图层不绘）、D4（面板删除不撤声明→
  「已删」受管层复活）、D5（切页回放展开态快照）、D6（declared() 全表
  SQL 扫描的规模热点）、D7/D8/D9——本轮未动：D4 是行为语义决策（删除
  =注销 vs 暂时移除需产品裁决）；D6 属性能改造需独立 A/B 基准轮；其余
  低危。已在本文记录，供后续任务选块。
- R2 盘点的属性表面板切层无会话闸门、DraftAutosave 失败路径零测试、
  commitErrors 丢弃、stopEditing 裸 QMessageBox、析构兜底、跨层拓扑
  约束层参与、外来会话状态标签漂移——未动（attributetablepanel 文件
  归属需与 #410/#432 的占有方划界；裸 QMessageBox 修复牵涉 #410 测试
  缺口边界）。记录在案。
- R4 盘点的 semanticlegend layerOrder 收未勾选层（当前无生产消费方）、
  mapbook manifest dpi/format 断言、mapbooklayout 未收敛 standardelements
  单一源——未动，低危/漂移风险级。
- Block 6 的 stale catalog 回滚分支未覆盖（见上）。
- 真机视觉（浅/深主题、1100×700 窄面、HiDPI）未在本轮人工核——offscreen
  断言已覆盖行为面；如实记为待人工可视检查。

## 全量 ctest（HEAD）vs master 基线

（待填：两轮结果与红集对照）
