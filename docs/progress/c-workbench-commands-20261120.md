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
  （C-W2 统一准入：基础上下文→切换中→依赖位→只读，同一函数服务触发时刻与
  模态返回后复核）。
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

（构建/测试后追加）
