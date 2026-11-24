# I3 进度：受管图层与地图显示原子（paleo.map_layer*）

任务书：显示与交互原子化之 I3。分支 `codex/paleo-i3-agent-map-display-atoms-20261123`，
worktree `../paleo-i3-agent-map-display-atoms`，构建片段
`cmake/extra-i3-agent-map-display-atoms.cmake`。开工基线 = `origin/master`
`63b26e784a475b17c734218b5bf53be7dbc9def9`（= 调研快照，fetch 后无新提交；
开放 PR 0）。

## 交付内容

五个精确命名的 Agent 工具（契约版本 `paleo.map-display.v1`）：

| 工具 | 效果 | 说明 |
|---|---|---|
| `paleo.map_layers` | Read | manifest 受管层目录 + 可观察显示状态；分页（1–32/页，缺省 16），无状态摘要游标绑定显示修订与筛选；查询不实例化、不切层位、不建画布 |
| `paleo.map_layer_get` | Read | 单层显示详情：树勾选/effective 可见（父组隐藏原因）、透明度、当前具名样式、语义锚、编辑态、适用预设与语义选项（几何适配）；未实例化如实标记 |
| `paleo.map_layer_set_visibility` | Read\|Presentation | 原生树节点 checked 设置 + 回读 checked/effective；不代开父组；同值 no_change |
| `paleo.map_layer_set_opacity` | Read\|Presentation | QgsMapLayer::opacity() 0–1 + 回读；只改呈现；编辑中/非矢量栅格明确拒绝 |
| `paleo.map_layer_apply_style` | Read\|Presentation | 具名预设（现存 styleManager 按名）或语义符号（GeoPatterns family/id 几何适配）互斥二选一；回读 currentStyle/renderer/语义锚 |

共同语义：

- 修改经宿主 `Read|Presentation` 效果门（`host::checkContext` live 复核：
  文档/世代/只读/写锁/恢复态），模型不可经实参自授；查询只须 Read。
- `state_token` 是「当前实例身份 + 显示事实」的无状态哈希摘要（材料含
  QGIS 实例 id，同 layerId 换工程/换实例必失配）——不是授权凭证；修改
  回执返回新 token 供链式调用；历史回放零 GUI mutation。
- 回执区分 `applied` / `no_change` / `state_token_stale` /
  `state_changed_during_apply` 等可证明状态，`persistence` 恒为
  `unsaved_project_display`；成功修改 `QgsProject::setDirty(true)`，
  工具从不写 .qgz/manifest/Catalog/操作日志。
- I4 隔离：分派用 `isMapDisplayTool` 精确集合（`ai/chat/mapdisplaytools.h`
  单一权威），无任何 `startsWith("paleo.map_")` 分支；`paleo.map_locate`
  在本 PR 保持 NotFound。

## 文件清单

主写新文件：

- `src/ai/chat/mapdisplaytools.{h,cpp}` — 五工具 AiToolSpec + 精确名称集合。
- `src/workflow/agentmapdisplay.{h,cpp}` — 严格外部档补齐校验（互斥/形状/
  长度界）；`Access` 接口声明在 qgis 头（见下）。
- `src/qgis/agentmapdisplayadapter.{h,cpp}` — `paleo::agent::mapdisplay::
  Access` 纯 Qt 接口 + 生产适配器（快照/可见性/透明度/样式/回读/游标）。
- `src/workflow/aichattoolrunner_display.cpp` — runner 分支 + `setMapDisplay`。
- `src/app/agentmapdisplaywiring.{h,cpp}` — 组装根安装（幂等，AppContext
  同寿命）。
- `tests/tst_i3_map_display_atoms.cpp` — 真实 runner 链测试 + 手动 GUI 证据
  场景（`PALEO_I3_GUI_EVIDENCE_DIR` 触发）。
- `cmake/extra-i3-agent-map-display-atoms.cmake`、本文件。

共享文件最小修改：

- `src/ai/chat/domaintools.cpp` — 追加注册一行 + dispatch 精确路由分支 +
  include（约 +12 行）。
- `src/workflow/aichattoolrunner.{h,cpp}` — `runMapDisplayTool`/`setMapDisplay`
  窄声明与成员 + startNext 一处分派。
- `src/workflow/aichattoolrunner_readiness.cpp` — map display 恒走 hosted
  资格（不落入无 Host 的旧适配器默认放行）+ provider/工程可用性分支。
- `src/qgis/qgiscanvascontroller.h` — `canvasIfCreated()` 只读观察缝（3 行，
  不触发惰性创建）。
- `src/app/main.cpp` — attachWorkbench 后一行安装 + include。
- `CMakeLists.txt` — `_paleo_extra_manifest` +1。
- `tests/tst_aichat.cpp` / `tests/tst_aichatprojectquery.cpp` — 工具名单 +5、
  计数 26→31。
- `translations/paleo_{zh_CN,en}.ts` — 20 条 QObject 上下文条目。

## 架构边界

- `src/workflow` 新文件零 Qgs 符号（qgs-audit/qgis-spread 闸）；QGIS API
  全部留在 `src/qgis/agentmapdisplayadapter.*`；接口面纯 Qt 类型。
- 适配器为 QObject（AppContext 子对象），所有 QGIS 调用断言所属线程；
  每次 resolve 重找树节点，回读不吃旧指针（QPointer 守卫同步回调路径）。
- 无长期观察连接（状态标记/游标是无状态哈希）——「临时记录数量与寿命
  上限 = 0」。

## 验证记录

环境（同宿主串行，重构建锁 `/tmp/paleo-i3-map-display-rebuild.lock`，构建
`-j4`、测试 `-j1`）：

- 基线：`origin/master` = `63b26e78`（fetch 后无新提交；开放 PR 0；
  `codex/paleo-p5-edit-lifecycle-20261119` 远端分支及其占用域未触碰）。
- 依赖路线：deb 闭包前缀（`vendor/prefix/usr` → h6 准备的闭包，带
  `.paleo-deb-closure.sha256` 收据）；主仓 superbuild 前缀的
  `debian13-base.lock` 收据不匹配，按 BUILDING 不可用于本分支 configure。
  `vendor/ranuts-document` symlink 接通（configure `office-editor 已就绪`）；
  `vendor/Data_service` 按主仓 gitlink `905b5c1` 克隆。
- 本机怪癖（与兄弟任务同款）：Arch 宿主需显式
  `CMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`；构建/测试期 Qt 工具链与
  运行库需前缀 `LD_LIBRARY_PATH`（rcc 的 Qt6Core 冲突）；本机 ctest 与
  前缀老 expat 不兼容，以 `LD_PRELOAD=/usr/lib/libexpat.so.1` 解决
  （a-async-previews-20261120.md 同款记录）。

构建与测试（实际命令与结果）：

- `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DQGIS_PREFIX=<deb prefix>/usr -DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu`
  —— configure 成功；`cmake --build build -j4 --target paleo_ai paleo_workflow
  paleo_qgis tst_i3_map_display_atoms tst_aichat tst_aichatprojectquery paleo …`
  全部编译通过（修复过程：`QgsMapLayer` 前向声明、`qgsrenderer.h` 头名、
  `QJsonArray` include、`parent()` 非 const、namespace 别名捕获、
  `QStringLiteral(变量)`、`takeChildNode` 不存在→clone 挪节点）。
- `./paleo-dev test -j1 -R tst_i3_map_display_atoms`：**19/19 通过**
  （发现并修复实现 bug 一处：`list()` 分页漏 offset 跳过——第二页重复
  首页条目；测试侧修正 token 跨层串用/预设删除后 token 失效时序两处）。
- 受影响既有面（补跑）：`tst_aichat`、`tst_aichatprojectquery`（名单 +5、
  计数 26→31、map_layers 无必填例外）、`tst_agent_tool_contracts`、
  `tst_agent_tool_readiness`、`tst_aiwiring`、`tst_layerservice`、
  `tst_canvas_tools`、`tst_geosymbols`、`tst_layerplatform`、
  `tst_layertreepanel`、`tst_layerproperties`、`tst_h4_operation_discovery`、
  `tst_h4_operation_list_tool` —— 全部通过；`tst_aichattoolloop` 单独跑
  通过（146s），但在长车道中两次于 300s 超时，挂点
  `queryProjectLoopAnswersWithWellNames`（loopback HTTP 回合，负载敏感；
  本 diff 不触该路径，非断言失败）。
- 源码闸门：`check_layering.py --strict` 无违规；`--qgs-audit --strict`
  134=基线新增 0；`check_qgis_spread.py --strict` 169=基线新增 0；
  `check_file_budget.py --strict` 51=基线；`check_i18n.py` 通过；
  `--transitive`（QGIS_PREFIX 注入）功能层 QtWidgets 黄牌 0；
  ctest 闸门 `ui_size/ui_tokens/qgis_versions/docs_drift/ci_known_reds/
  cpp_mainline(_selftest)` 13/13 通过。已知环境项：`first_configure` 在本机
  红（其内部 configure 不带本机必需的 `CMAKE_LIBRARY_ARCHITECTURE`，
  Arch 非 multiarch 宿主怪癖；H4 记录同款 62/63）。

GUI 证据（有窗口人工观察，非 offscreen 冒充）：

- `PALEO_I3_GUI_EVIDENCE_DIR=… DISPLAY=:0 ./build/tst_i3_map_display_atoms`
  （xcb 真窗）：19/19 断言 + 8 张截图存
  `docs/evidence/i3-map-display-atoms-20261124/`
  （01 初始地图+图层树、02 目录查询、03 单层状态、04 隐藏线层、05 栅格
  透明度 0.45、06 具名预设、07 语义符号（面层岩性花纹）、08 只读/伪造
  身份拒绝态）。像素差程序化复核：04 vs 01 变化 27.0%、05 vs 04 变化
  22.6%、07 vs 06 变化 20.7%——每步画面确有可见变化，与回执一致。
- offscreen 车道（CI）只跑断言，GUI 场景不触发。

## 独立审查记录

R1（未参与实现的 general-purpose agent 读真实 diff + 实跑闸门/测试）：
**零阻断项**；确认无虞项 10 条（权限/效果门/回读/幂等/I4 隔离/分层/测试
质量/翻译等）。建议项处置：

| 发现 | 处置 | 验证 |
|---|---|---|
| R1 owner 守卫恒假死代码（QPointer bool 语义），悬垂风险 | 修：`m_mapDisplayOwner.isNull()` 单判 + `setMapDisplay` 强制 access/owner 成对供给（无 owner 按未安装） | 新增测试：无 owner 供给 → readiness `map_display_provider_unavailable` |
| R2 具名预设分支 `setCurrentStyle` 同步信号后经旧 sm 回读的 UAF 顺序 | 修：先查 `layerGuard`，经 `layerGuard->styleManager()` 重取回读 | 既有 preset 用例全绿；顺序与语义分支对齐 |
| R3 semantic_symbol 嵌套未知键静默忽略 | 修：非 family/id 键拒绝（`unsupported_symbol_field`） | 新增负例测试 |
| R4 horizon 筛选无长度界 | 修：≤200（`invalid_horizon`） | 新增负例测试 |
| R5 长度界/opaque no_change 脏标记无测试钉住 | 补测：layer_id 201/state_token 65/horizon 201 越界拒绝；opacity no_change 后 `isDirty()==false` | tst_i3 全绿 |
| R6 displayRevision 双份实现漂移风险 | 修：`list()` 改用 `displayRevision()` 单一来源 | 游标失效用例全绿 |
| R7 note 字面量未过 tr | 保持（与 H4 机器回执口径一致；i18n 闸不覆盖） | — |
| R8 空 cursor 得 `cursor_expired` 码名语义 | 保持（无效游标统一报 expired，注释已写明） | — |

修复后复跑：`tst_i3_map_display_atoms` 19/19、受影响五测试 100%、GUI 证据
重新生成并复核像素差、闸门重跑全绿（见上）。

## 未覆盖项（如实）

- 未实现未实例化层的修改（`layer_not_materialized` 诚实拒绝——任务书
  允许初版不支持）；未实现 annotations 层透明度/样式
  （`unsupported_layer_type`/`semantic_symbol_unsupported_geometry`）。
- 状态标记并发变更检测为「摘要失配即拒」粒度：同层其它显示事实被外部
  改动会让旧标记失效（导向重新查询），不做字段级 diff。
- `queryProjectLoopAnswersWithWellNames` 在本机长车道中的 300s 超时未在本
  PR 解决（负载敏感的环境项，solo 直跑/单独 ctest 均通过；本 diff 不涉
  及该路径）。
- Windows 侧未验证（本任务在 Linux 宿主完成；跨平台车道由 CI 承担）。
