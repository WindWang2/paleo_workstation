# C｜工作台命令资格、页面反馈与绑定生命周期 — 任务账本

- 分支：`codex/paleo-c-workbench-commands-20261120`
- Worktree：`../paleo-c-workbench-commands-20261120`
- 基线：origin/master@1a79b745（2026-11-20 快照后 #1051/#1054/#1055 已合入，
  开放 PR 仅剩 #1053 P4 ONNX——其边界（ONNX/predictionworkflow/predictpage/
  attach_predict）本任务不触碰）
- 平台：Linux x86_64（CachyOS，glibc≥2.43）；受控依赖前缀
  `/home/kevin/projects/paleo-deb-prefix-20261119/usr`（deb 闭包路线，收据
  `.paleo-deb-closure.sha256` 与当前 `vendor/deb-closure.lock` 哈希一致＝
  90a7bf14…，只读共享，configure 需显式
  `QGIS_PREFIX_PATH=… -DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`，
  或 `QGIS_PREFIX_PATH=… ./paleo-dev configure -DCMAKE_LIBRARY_ARCHITECTURE=…`）
- 关联 issue（全部仍开放）：#983 #987 #988 #994 #995 #996 #997 #999 #1000

## 开工核对结论（源码观察，非运行复现）

| 包 | 当前生产调用链 | 已具备能力 | 待验证缺口 | 相关 PR/branch |
|----|--------------|-----------|-----------|---------------|
| W1 | workbenchcmd::knownActions 字符串集合；handleCommand 先试 EditFlow→Query→Edit→Lifecycle 四段 Skip 链 | 未知动作显式失败（H1）；tst_mappingworkbench 按钮⊆登记表＋正则扫分派源码 | 登记表/分族/准入/结果文案平行维护；正则检查可被同文本绕过 | #920（命令框架） |
| W2 | 页面按钮经 issue→commandRequested→controller；ribbon 镜像经 bindMirror+dispatcher | dispatcher 统一触发、surface 拒绝、禁用带原因 | controller 入口无只读/会话/依赖准入；`d.show` 等可空回调直接调用可 bad_function_call | — |
| W3 | DataListPanel::undoOp/redoOp 仅看 m_opStack 可操作性；局部快捷键 data.list.undo/redo 直调 | FaultManagerPanel/StratigraphyWorkflow/router 已有只读防御（复用验证） | 数据列表局部撤销完全未接宿主写许可 | — |
| W4 | Context 部分 QPointer、部分裸指针；删除确认后直接用 canvasCtl/wellPanel/wellDock/shown | exportVector 已有 session 再验证模式（复用） | 删除确认/导入对话框返回后无宿主/工程再校验 | — |
| W5 | attachCanvasFaciesMenu 固定捕获 pages.value(0) 发 pinFaciesLevel | 无页（测试壳）有后端直调兜底 | 反馈页面错误归属首个工作台页；目标与反馈页混同 | — |
| W6 | ButtonMirror 删除旧 mirror 建 new；toggled 连接 context=action | 带 paleoCommandId 时 dispatcher 唯一激活、显式 checked 目标态 | 旧源 toggled 残留影响；checkable 从不复位；源缺失/销毁无诊断；绑定表 page=1 永远 skip | — |
| W7 | attachWorkbench 幂等守卫在函数开头即置 workbenchAttached=true | attachWorkflows 独立「成功才置位」契约（保持不动） | 前置失败不可重试；productReady/queued 回调解引用 m_selection 不判空 | — |
| W8 | tst_commandshell/tst_mappingworkbench/tst_shortcuts_shell 已有闸 | 镜像目标态、子菜单弹层、未知拒绝回归 | 无准入/重绑/跨模态生命周期专属回归；无 GUI 证据 | — |

占用/避让：A（预览服务注入/排空）、B（脚本进程）、P4 open PR #1053、
P5/P6 已分配未回归引擎——均不修改。predictpage/runConstraintPageIdw 不动。

## 实施记录

### 基础设施
- 新文件 `src/ui/workbench/workbenchactioncatalog.{h,cpp}`（C-W1 单份动作描述：
  id/family/effect/outcome/caps，43 条动作表 + catalogIntegrity 自检 +
  missingCapsReason 点名原因）；`src/ui/workbench/workbenchadmission.{h,cpp}`
  （C-W2 统一准入：基础上下文→切换中→只读→依赖位（只读先于依赖位，全局资格
  不被面板缺席遮蔽），同一函数服务触发时刻与模态返回后复核）。
- 挂点：`cmake/extra-c-workbench-commands.cmake`（target_sources paleo_ui +
  add_paleo_test(tst_c_workbench_commands LIBS paleo_ui)），根 CMakeLists
  `_paleo_extra_manifest` 追加单条。
- 新测试 `tests/tst_c_workbench_commands.cpp`（13 用例，见验证记录）。

### C-W1（#983）
- `workbenchcommandregistry.h`：knownActions() 改为从 catalog() 派生（删 43 条
  平行字符串）；pageLocalActions 不变。
- `mappingworkbenchcommandcontroller.cpp` handleCommand：按 spec->family 直达
  唯一处理入口（switch），删除「EditFlow→Query→Edit→Lifecycle 依次试探」链；
  族内漏分支落入显式失败「未被其声明处理族受理（登记与实现不一致）」。
- `tst_mappingworkbench.cpp`：正则扫源码检查替换为 catalogIntegrity +
  knownActions==catalog 结构校验；运行时分派覆盖移到 tst_c_workbench_commands
  逐条真实分派验证。

### C-W2
- Context 新增 `projectReadOnly`/`projectSwitching` 触发时刻钩子（editflow 接
  isProjectReadOnly()/m_projectSvc->isOpening()）。
- Effect::Mutating 在只读工程拒绝（predict/recompute/wellattributes 等查询族
  产结果动作按实际副作用记 Mutating；cancel/show/graph/catalog/导航/导出记
  Read——只读工程可浏览查询导出）。
- 基础上下文缺失从静默 return 改为尽力回报（statusBar/ErrorHub）。

### C-W3（#995）
- `datalist.{h,_undo.cpp}`：setUndoReadOnly(bool) 兼容加法；refreshUndoButtons
  只读门优先（置灰+tooltip 原因，不假装「无可撤销」）；undoOp/redoOp 入口
  复核（按钮/快捷键/直调同一条门），不清栈。
- `paleomainwindow_undogroup.cpp` updateUndoProjectGate：DataListPanel 并入
  router/fault/seismic/layout 同一轮通知。
- FaultManagerPanel/StratigraphyWorkflow/router 既有防御不动，router 复用验证
  见 tst（setWritable(false) → step 拒绝且栈位置不变）。

### C-W4（#988）
- Context 可销毁 QObject 全部 QPointer 化（canvasCtl/layerSvc/edit/constraint/
  wellPanel/wellDock）。
- postModalUsable(d, action, &error)：删除确认/导入文件与角色选择/导出对话框
  返回后统一重过 WorkbenchAdmission::admit（宿主/页面/依赖/只读/切换），
  exportVector 既有 session 复核保留并叠加。

### C-W5（#997）
- PaleoMainWindow::currentWorkbenchPage()：按当前可见页签解析所属
  MappingWorkbenchPage（objectName mappingWorkbench.<mode>），非工作台页为 null。
- attachCanvasFaciesMenu：不再固定捕获 pages.value(0)；pin-level 回调触发时解析
  当前页发 commandRequested（目标 layer id 仍为右键冻结对象）；无页测试壳
  直调兜底先过只读门（不再保留绕过 gate 的捷径）；「更改此要素的相」入口
  同过只读门。
- attachWorkbenchWells workspaceRequested/activeHorizonChanged：m_selection
  判空（#994）。

### C-W6（#996/#1000）
- ButtonMirror：toggled→action 连接改以镜像为接收者（删除镜像即断开旧源）；
  checkable 双向完整复位（不可勾选源 → action 复位非勾选）；源 destroyed →
  动作禁用+可诊断 tooltip。
- mirror()：源缺失（null button）→ 禁用+原因（不再静默 no-op 保留旧可用态）。
- 绑定表删除 page=1 永远 skip 的 ribbonDrawConstraint/ribbonRunIdw 死配置，
  注释明确 ConstraintPage 独占 draw/factor 方法入口（防 IDW/绘制双执行）。

### C-W7（#999）
- attachWorkbench：workbenchAttached 完成标记从入口移到全部接线成功后
  （前置失败可重试）；preflight 增加 m_canvasCtl；m_selection 判空覆盖
  productReady/queued 切层回调。attachWorkflows 契约不动。

## 验证记录

平台：Linux x86_64（CachyOS，xcb 真实 GUI + offscreen 断言）；
base = origin/master@1a79b745；head 见 PR。
前缀：`/home/kevin/projects/paleo-deb-prefix-20261119/usr`（deb 闭包，收据一致，只读共享）。
构建：`QGIS_PREFIX_PATH=… ./paleo-dev build`（PALEO_JOBS=4，主机级锁）。

| 验证 | 命令 | 结果 |
|---|---|---|
| 专属回归（offscreen） | `./build/tst_c_workbench_commands`（3 次连跑） | 18 passed / 0 failed（3 GUI demo 用例 offscreen 跳过） |
| 专属 GUI 演示（xcb 真窗） | `PALEO_C_GUI_DEMO=1 PALEO_C_SHOT_DIR=… tst_c_workbench_commands guiDemo*` | 5 passed / 0 failed；11 张截图存 `docs/evidence/c-workbench-commands-20261120/` |
| 既有工作台套件 | `tst_mappingworkbench` | 138 passed / 0 failed / 1 skipped |
| 命令入口闸 | `tst_commandshell` | 19 passed / 0 failed |
| 快捷键闸 | `tst_shortcuts_shell` | 15 passed / 0 failed |
| 壳层套件 | `tst_ui` | 44 passed / 0 failed / 8 skipped |
| ribbon 族 | `tst_ribboncollapse` / `tst_ribbontitlefit` | 10 / 15 passed |
| 数据列表面板 | `tst_assetpaging` | 13 passed / 1 skipped |
| Source gates | `ctest -R "layering|qgis_spread|ui_invariants|ui_size|file_budget|i18n|ui_tokens|qgis_versions|docs_drift|cpp_mainline"` | 29/29 Passed（attachWorkbench 拆出 mirror 段后回到基线 276 冻结值，无回升） |

### GUI 三条路径摘要（真实 xcb 窗口 + 程序断言 + 截图）

1. **数据列表撤销门**（c8-path1-*）：可写历史建立 → 切只读（按钮置灰+原因
   tooltip）→ 真键盘 Ctrl+Z（`undone==0`、历史保留）→ 恢复可写 → 撤销成功
   （`undone==1`）。
2. **工作台页面反馈归属**（c8-path2-*）：同一 show 意图在 predict/compose
   可见上下文触发，反馈页随 resolver 变化；切到 data 页 resolver 为 null
   （不落到隐藏工作台页）；切回 constraint 命中对应页对象。
3. **镜像生命周期 + 只读确认负例**（c8-path3-*）：主源↔换源↔切回（旧源
   继续变化不影响动作）；专用演示源销毁 → 动作禁用+诊断 tooltip；恢复镜像
   后状态跟随源真实业务态；删除确认弹出期间切只读 → 接受键返回后按只读
   拒绝（feedback 含「只读」、图件仍在）。

### 测试基建发现（如实记录，未改产品行为）

- `PaleoNotify::ask` 的 offscreen 无头守卫（paleonotify.cpp B-05）固定 50ms
  自动按默认键关闭对话框——测试协作必须先于它命中（ModalCloser 10ms 轮询）。
- 真实 xcb 上 exec 早期的 QMessageBox 可能未被 WM 标记 visible（匹配条件需
  含 isModal）；被拒路径的 `PaleoNotify::warning` 在真实平台同样模态（offscreen
  有守卫），演示需后续模态看护。
- tst_shortcuts_shell 偶发 `scriptrunner.cpp.o` debug_info 重定位截断
  （R_X86_64_32），重试链接即过——与 A/B 并行构建共享 ccache 有关，非本任务
  改动引入（该 TU 不在本次 diff 内）。

## 未覆盖/限制

- Windows 平台未验证（本机 Linux；NativeFormat QSettings 注册表行为不在范围）。
- #987 全局 findChild 收敛不在本任务范围（仅 types 化 currentWorkbenchPage
  一个 resolver）；#995 的主窗文本编辑焦点原生 Ctrl+Z 优先级由既有
  ShortcutCatalog 语境保证，未新增第二套键序。
- locator（Ctrl+K）入口的准入一致性依赖 CommandDispatcher 的 actionState/
  bindCommand state（既有机制），本轮未改其协议；工作台 controller 直调
  路径已过统一准入。
