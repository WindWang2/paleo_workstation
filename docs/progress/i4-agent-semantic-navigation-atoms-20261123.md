# I4：基于业务身份的井、地震与地图导航原子——实施记录

日期：2026-11-24。分支：`codex/paleo-i4-agent-semantic-navigation-atoms-20261123`
（基线 `origin/master` = `63b26e78`，与任务书调研快照同一提交）。
Worktree：`../paleo-i4-agent-semantic-navigation-atoms`（独立 build/）。

## 0. 范围与复用边界

六个 Agent 导航原子落地为生产工具：`paleo.navigation_state` /
`paleo.select_wells` / `paleo.clear_well_selection` / `paleo.map_locate` /
`paleo.open_well_view` / `paleo.open_seismic_line`。效果恒
`host::Read`（临时选择/视图导航），不写 Catalog 业务状态、不改
Presentation 写门、不改 renderer/可见性/样式（I3 域）。复用既有
SelectionContext 选择总线、WellMapLink 业务 ID 索引、
QgisCanvasController/projectmapreference 转换、DataPreviewTabs 原生预览与
共享 PreviewDocService；未重做 H1–H7/G5 任何成果。

与 I3（PR #1076，open）的路由边界双向钉死：本族 `isNavigationTool`
精确六名，I3 `isMapDisplayTool` 精确五名，两侧都无 `startsWith("paleo.map_")`
吞路由。共享接缝（domaintools.cpp / aichattoolrunner.{h,cpp} /
aichattoolrunner_readiness.cpp / main.cpp / 根 CMakeLists）只加本任务增量。

## 1. 生产实现落点

| 文件 | 职责 |
|---|---|
| `src/ai/chat/navigationtools.{h,cpp}` | 六工具严格 schema/枚举/上限（新 `paleo_ai`） |
| `src/services/agentnavigationaccess.h` | SelectionAccess/MapAccess/PreviewAccess 纯 Qt 契约（数据层——qgis 实现方不得反向 include 功能层，避免模块依赖环） |
| `src/workflow/agentnavigation.{h,cpp}` | 受信目标解析（W2）/严格校验共用入口/单在途请求 + 60s 截止 + 迟到回执丢弃（W8） |
| `src/workflow/aichattoolrunner_navigation.cpp` | runner 分支：宿主 Read 效果门复核（live context）+ 委托编排 |
| `src/qgis/agentmapnavigation.{h,cpp}` | MapAccess 生产实现：井包络/受管图层严格 CRS 定位、extent 回读核含、隐藏目标如实报 |
| `src/ui/agentnavigationadapter.{h,cpp}` | SelectionAccess + PreviewAccess 实现：agent_navigation 可信井来源广播 + 双总线回读；typed 预览委托 |
| `src/ui/datapreview/datapreviewtabs_navigation.cpp` | DataPreviewTabs typed 接缝：固定版本井资料三事实回读（W5）、地震测线真实显示完成/顶替/取消（W6/W8）、per-request SHA 策略转发（W7） |
| `src/ui/paleomainwindow_agent_navigation.cpp` + `paleomainwindow.h` | `attachAgentNavigation` 窄工厂（幂等；不开放任意 QWidget 查询） |
| `src/app/agentnavigationwiring.{h,cpp}` + `main.cpp` | 组装根安装（画布/图层/工程服务 + 主窗适配器 + runner） |
| `src/linkage/seismicmaplink.{h,cpp}` | `isWellSelectionOrigin` 井域 origin 隔离（well_map/datatree/datapreview/agent_navigation/rebind 不扫/不清地震线层） |
| `src/linkage/wellmaplink.{h,cpp}` | `selectedWellFeatureCount()` 只读回读出口（ui 不直连 qgs* 头） |
| `src/services/previewdoc.{h,cpp}` | ShaGatePolicy（per-request 只读策略）+ 版本精确 SHA 缓存 + `externalShaVerified` 查询 |
| `tests/tst_i4_navigation_atoms.cpp` | 17 用例（§4） |

薄接缝（既有文件最小修改）：`datapreviewtabs.{h,cpp}`（typed 声明 +
attachDoc 挂回执路由 + closeAssetTab 收口）、`datapreviewtabs_well.cpp`
（wellNotInFileText/noTimeDepthText 观察标记，file-budget 冻结内），
`datapreviewtabseismic.cpp`（decode 的 per-request 策略透传，单行），
`datapreviewtabs_content.cpp`（外链闸门策略透传），
`qgiscanvascontroller.h`（`canvasIfCreated()` 只读观察缝——不触发惰性
创建），`aichattoolrunner_readiness.cpp`（导航族 hosted 资格分支），
`domaintools.cpp`（六工具注册 + 精确名分派）。

## 2. 关键语义决策

- **井选择**：整批先在 catalog 核实全部为井实体，未知/错类型整批拒绝
  （无部分选中）；`setSelection(ids, "agent_navigation")` 走既有总线，
  WellMapLink 复用业务 ID 索引 + QSignalBlocker；回读双事实（总线
  selectedIds + 井层原生选中集）。重复 ID 去重保序。
- **跨类型隔离**：SeismicMapLink 对井域 origin 直接返回——选井/清井不
  再扫描地震线层，同字串 line_id 不被误选/清空；覆盖 `open_well_view`
  复用的旧 `wellSelected → origin=datapreview` 广播（普通 UI 行为不变，
  地震域自身 origin 不受影响）。clear 的异域判定：当前选择含非井目标 →
  `foreign_selection` 不动作；token 失配 → `stale_selection`。
- **地图定位**：坐标源 = catalog surfaceX/Y（工程网格局部米，与
  zoomToPoint 同源）；逐点经 mapreference::transformPoint 严格变换，失败
  即拒绝且旧 extent 不动（不沿用 zoomToLayer 的原始范围回落）；fit 外扩
  1.08，零宽点 ±500m 缓冲；setExtent 后回读核含目标包络；隐藏目标
  （layer tree 未勾选/层未实例化）如实报 `located_not_visible` /
  `layer_not_present`，不改可见性。
- **井资料**：版本归属 + 类型显式表（well_head/well_stratification/
  time_depth）+ 井关联已决三重校验后才开页；外链 SHA 未验先经验证
  （ReportOnly）再打开；三事实（page_open/page_focused/正文 shown）分开
  回报，正文状态按稳定标记分类（error/no_resolved_wells/well_not_in_file/
  select_well/shown）。
- **地震测线**：survey 冻结几何有界预检（拒绝先于任何控件写入——不吃
  SpinBox 钳位）；目标解码经真实控件链路，完成按目标 (axis,line) 认领：
  shown（终态）/partial（振幅已显示、层位仍在读取——诚实区分）/
  failed（解码或 SHA 失败原文）/superseded（用户换线；建页初始解码噪声
  经 armed 位不算顶替）/cancelled（关页收口）。迟到回执不产生第二终态。
- **版本精确 SHA 闸（W7）**：`m_shaVerified` 键从 assetId 改为
  `assetId|versionId|sha256`——同资产他版本/他指纹的已验结论（含晚成功）
  不放行本版本；失配副作用按 per-request 策略（普通 UI 缺省仍
  markDownstreamStale，既有行为与 tst_datapreview 的失配用例保持绿）；
  Agent 只读导航 ReportOnly——实测 catalog revision 前后不变（零写）。
  resetSha/releaseSection 按资产前缀清全部版本键。
- **Read 语义**：允许既有受控预览读取/任务/缓存（正常预览可用共享
  TaskService 与 reader 缓存）；不新增/发布业务成果、不自动保存工程。

## 3. 本地验证

- 配置：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu
  -DQGIS_PREFIX=<worktree>/vendor/prefix/usr`（deb 闭包前缀 symlink 自
  paleo-h3 worktree → h6 prefix；Data_service 按主仓 gitlink
  905b5c1b 本地克隆；office-editor configure 期就绪无警告）。
- 构建：`LD_LIBRARY_PATH=<deb-prefix>/usr/lib/x86_64-linux-gnu
  cmake --build build -j4`（vendored rcc 需闭包 Qt；全程 -j4，构建锁
  /tmp/paleo-build-locks 自 I1 闲置后接管，README 记录移交）。
- 测试：`QT_QPA_PLATFORM=offscreen ctest -j1`（详见 §4）；新增
  `tst_i4_navigation_atoms`（add_paleo_test LIBS paleo_workflow paleo_qgis
  paleo_ai paleo_ui，最小链接集）17/17 通过。
- 源码闸门（本地全绿）：layering --strict / layering --qgs-audit
  （134=baseline，新增 0）/ check_qgis_spread（169=baseline，新增 0）/
  file_budget --strict（回升 0）/ check_i18n / check_l10n（新词条已补
  en/zh_CN 双语，含禁用术语修正）/ ui_size / ui_tokens / docs_drift
  （W8 家族表现状行刷新为 35 TU）/ qgis_versions。
  `--transitive` 依赖 QGIS 头路径的权威闸口在 CI lint 车道（本地未注入
  QGIS_PREFIX 的 ctest 项不替代，AGENTS.md 口径）。

## 4. tst_i4_navigation_atoms 用例（17）

1. `toolRegistryExposesExactSixNames`——六名注册/effects=Read/Callable/
   contract 常量/schema additionalProperties=false/无 timeMs。
2. `strictValidationRejectsBeforeAnyAction`——空数组/非字符串元素/超 64、
   map_locate 双目标同给与皆无、非法 axis、分数线号、缺 ID。
3. `dispatchRoutesExactNamesOnly`——六名 Routed；`paleo.map_layers` /
   `paleo.map_locate_v2` NotFound（不吞 I3 路由）；类型错 Disabled。
4. `selectWellsReplacesBusinessSelectionWithCrossDomainIsolation`——真实
   SelectionContext+WellMapLink+SeismicMapLink+内存层；地震线 ID 与井 ID
   同字串；agent 选择后总线/井层双回读一致、地震线选择保持 1、
   activeHorizon 不动；navigation_state token 回传清除成功且地震选择仍
   保持。
5. `selectWellsUnknownTargetsRejectedWhole`——含未知 ID 整批拒绝、原选择
   原样。
6. `clearWellSelectionHonorsTokenAndForeignDomain`——伪造 token →
   stale_selection；地震域选择 → foreign_selection 不清；空选择 →
   no_change。
7. `mapLocateWellsStrictTransformAndReadback`——fit 实际 extent 核含全部
   目标井坐标；center 保留宽高且中心命中；无坐标井整批拒绝且 extent
   不动。
8. `mapLocateRejectsMissingLayerAndInvalidExtent`——未声明层
   layer_not_present（不实例化）；空图层 layer_extent_unavailable；无效
   画布 CRS → coordinate_transform_failed 且旧 extent 不动（无伪坐标
   回落）。
9. `openWellViewTypedFactsAndVersionPinning`——多井分层资产双版本：v2
   打开 shown/三事实/actual version=请求版本；再指 v1 保真切换，同资产
   复用标签。
10. `openWellViewRejectsUnlinkedWellAndForeignVersion`——跨资产版本
    version_not_in_asset；未关联井 well_not_linked_to_asset（页面不建）。
11. `shaMismatchReadOnlyNavigationNeverWritesCatalog`——外链真实文件+坏
    指纹 → sha_mismatch；catalog revision 前后不变（ReportOnly 零写）。
12. `versionPreciseShaCacheDoesNotGrantOtherVersions`——v-good 已开后
    v-bad（同资产坏指纹）仍拒绝；页面仍显示 v-good。
13. `seismicLineRealDisplayLifecycle`——真实 TaskService 解码 IL1000 →
    shown/partial；实际版本/轴/线号与标题后缀 `vol.sgy · IL1000` 一致。
14. `seismicLineRejectsOutOfRangeAndReportsDecodeFailure`——越界同步拒绝
    （页面未建）；真实 SEG-Y+坏指纹版本 → 解码 SHA 闸失败 → failed。
15. `seismicLineSupersededByUserChangeAndCancelledByTabClose`——agent
    在途请求被用户换线顶替 → 单终态 superseded、用户画面保持 IL1001、
    无第二终态；在途关页 → cancelled。
16/17. initTestCase/cleanupTestCase。

GUI 证据（真实可见窗口）另有 offscreen 之外的记录：见 §6。

## 5. 已知边界与如实声明

- `open_well_view` 类型表仅基线三类井资料；well_log 及 Office/任意资产
  按显式表拒绝（扩表需先核实同链路，见任务书）。
- 地震线「范围内但稀疏不存在」不预扫 SEG-Y：越界预检 + 真实解码失败
  双闸（failed 带原文）；不取最近线、无钳位冒充。
- 无 survey 几何的地震资产回退 0–1000000 预检域（与 applyLineSpinRange
  同源口径）。
- 部分 shown 判定依赖本类自设稳定标记（retryBtn/deadEndText/topsTable/
  wellNotInFileText/noTimeDepthText）——只读观察，不做控件自动化。
- 顶层的 Workflow 级超时（60s）回包是「未在期限内确认」的如实状态；
  页面后台完成的事实由后续 navigation_state 查询，不改写已发终态。

## 6. 真实 GUI 验证

（offscreen 之外的证据——按任务书要求「原生 Qt/QGIS GUI 演示」记录；
PALEO_I4_GUI_EVIDENCE_DIR 流程同 I3 先例：手动触发、offscreen 车道只跑
断言。）

- 演示序列（`PALEO_I4_GUI_EVIDENCE_DIR` 触发 `guiEvidenceDemo`，xcb 真窗
  1280×900，3/3 PASS）：查询当前状态 → 选两口井（well-A1/well-A2，回读
  selected_count=2）→ 定位包络（extent=[-525.6, 3726.8, 9535.1, 8392.8]，
  CRS="Paleo local engineering grid"，目标井层未实例化 → 如实
  located_not_visible）→ 打开明确井资料版本（ver-4 + well-A1 → status=
  shown）→ 显示明确 inline（ver-8 / inline / 1000 → partial：振幅已显示、
  层位仍在读取——截图含真实剖面渲染）→ 只读工程 center 定位成功 →
  未知井（unknown_well_ids）与越界测线（line_out_of_survey_range，
  999999）各拒绝一次。
- 截图 7 张 + 结果 JSON transcript 见 `docs/progress/i4-nav-20261124/`；
  offscreen 车道该用例 QSKIP（真实窗口证据为手动流程，不冒充）。

## 7. 审查记录

独立审查（未参与主要实现的评审者读真实 diff + 契约 + 测试结果）结论与
逐项修复见 §8（发现→处理→验证）。

## 8. 验收命令清单（复现）

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu \
  -DQGIS_PREFIX=$PWD/vendor/prefix/usr
LD_LIBRARY_PATH=$PWD/vendor/prefix/usr/lib/x86_64-linux-gnu \
  cmake --build build -j4
QT_QPA_PLATFORM=offscreen ctest --test-dir build -j1 \
  -R 'tst_i4_navigation_atoms|tst_linkage$|tst_datapreview|tst_previewdoc|tst_agent_tool_contracts|tst_agent_readiness|tst_agent_tool_readiness|tst_aiwiring|tst_aichat$'
python3 tools/check_layering.py --strict
python3 tools/check_layering.py --qgs-audit
python3 tools/check_qgis_spread.py
python3 tools/check_file_budget.py --strict
python3 tools/check_i18n.py && python3 tools/check_l10n.py
python3 tools/check_ui_size.py && python3 tools/check_ui_tokens.py
python3 tools/check_docs_drift.py && python3 tools/check_qgis_versions.py
```
