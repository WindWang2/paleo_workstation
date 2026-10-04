# 方向 34 ledger — 新井部署与井网辅助

分支 `goal/well-siting-20261004`（worktree `.worktrees/well-siting`）。

## 批次记录

### 批 1+2：算法核（src/algorithms/wellsiting/）
- `wellsiting.h/cpp`：`paleo::wellsiting` 纯核——最近邻统计（桶格环形扩张索引）、
  采样场（域内格中心欧氏距离场 + 8 连通空洞）、GEOS 阶梯边界（格矩形并集）、
  候选生成（最大空洞圆心 + 规则网格 + 避让/边界内缩/距井门）、方案指标
  （空洞面积/覆盖率/域均距/井间最近邻/按层位加权井控密度）、单候选边际贡献
  （与「基线−方案」同栅格口径解析一致）。
- 口径诚实面：note 随报告返回（欧氏近似/栅格面积/边界仅显示/不称地质最优）。
- **过程中修掉两个真 bug**：
  1. 桶格环形扩张在扫环 r 之前判停（`best ≤ r·h`）——环 1 点可任意近
     （下界 0），漏扫近井 → 距离虚高、空洞面积 7 倍虚胖。判停移到扫完环 r 后。
  2. GEOS 线性环要求闭合——矩形外环 4 角未回起点，union 全空。补闭合点。
- 测试 `tst_wellsiting_kernel`（12 用例）：Oracle 1（合成规则井网+人工空洞 →
  面积/位置/连通性 + 独立暴力参照 5% 内一致）；Oracle 3（加候选井方向+幅度
  解析断言：空洞清零/密度 9/100 口每 km²）；候选（圆心/网格/避让单调/禁钻/
  边界内缩/上限截断）；吞吐比率门（大格网 ≥ 参照小格网一半——无绝对毫秒墙）。

### 批 3：planned 实体登记 + 持久化 + 编排
- 词表正式登记：`datacatalog.h` 类型注释 + `roleregistry.cpp` planned 角色段
  （siting_note 部署依据 + other 兜底）；持久层 TEXT/JSON 天然开放零改动。
- UI 词表面：实体新建下拉 + 坐标行（well|planned）、entitypanel 创建前缀
  `planned-`/kindText、datalist「计划井」独立分组（与实井组隔离）。
- `src/domain/wellsitingplan.{h,cpp}`：方案纯值类型（含 retired/renames 可见性面
  ——catalog 实体不可变，删除/改名落 siting 文档，catalog 留审计痕迹）。
- `src/metadata/wellsitingstore.{h,cpp}`：单行 JSON 文档表
  `well_siting_scenarios`（FaultSetStore 同式，写经 enqueueWrite）。
- `src/workflow/wellsitingworkflow.{h,cpp}`：编排——实井收集
  （**只认 entityType=="well"**，隔离红线）、域解析（polygon 约束 → 地震角点 →
  井位包围盒外扩 R，来源如实注记）、诊断图层 `wellsiting_holes`（07_Validation，
  horizon=""不进版本快照）、planned CRUD + `planned_wells` 图层（00_Data）、
  评估+贡献分、方案快照、CSV/PNG 导出。
- `QgisStyleService`：applyPlannedWellLayerStyle（空心橙虚线方框+名注）/ 
  applyHoleLayerStyle（橙 25% 填+虚线边）。
- 测试 `tst_wellsiting_workflow`（10 用例）：Oracle 2（planned 隔离——
  realWellsForSiting/catalog.entities("well")/ProjectDataFacade 三面断言不含
  planned + 诊断/评估井数只数实井）；Oracle 4（保存-重开：catalog new 实例 +
  store new 实例，点位/指标快照/改名一致）；端到端（图层声明 07_Validation、
  域回退注记、避让端到端、评估幅度、导出 CSV/PNG 魔数）。

### 批 4：交互 UI
- `src/ui/maptools/sitingpicktool.{h,cpp}`：单发拾取工具（snapPoint+SnapIndicator，
  Esc 取消，拾取后自动退工具）。
- `src/ui/pages/wellsitingpanel.{h,cpp}`：布井辅助面板——诊断参数/摘要/空洞表/
  口径注记、候选表（贡献列 mono 数字面）、计划井表（增删改名+地图布点意图）、
  实时评估（plannedWellsChanged → 指标刷新）、方案对比表、导出意图信号。
  禁用按钮带 reason（§35）；DESIGN.md token 走 paleotheme/pageshared。
- 壳接线：`attachWellSiting`（验证页收成「验证/布井辅助」双页签——页 id 不变、
  ribbon/图层档案映射不受影响；地图工具/文件对话框归壳）。AppContext：
  store 值重绑（FaultSetStore 同式）+ 约束/断层切割 provider 注入。
- 测试 `tst_wellsiting_panel`（4 用例）：锚点齐全、意图信号、布点回调入 catalog
  刷表+实时评估、方案保存进对比表、口径注记上屏。

### 批 5：导出
- CSV 点位表（表头+名称/坐标/贡献）；对比图 PNG（基线+各方案空洞面积条形 +
  覆盖率/均距行 + 口径注记脚注；色值与 DESIGN.md token 同源，文档产物直绘）。

## 门禁与回归
- 新测试 ×3 全绿；`tst_arearules`/`tst_workflows`/`tst_constraintstore` 绿；
  layering×3 / ui_invariants×3 / i18n×2 全绿；`git diff --check` 干净。
- 全量 ctest 见 PR body。

## 多轮 review（硬要求）

- **轮 1**（独立复审 agent + 自审）：零 High；4 个 Medium（M1 GEOS 边界提取
  无预算且评估路径全浪费 / M2 计划井可见性双轨 / M3 对比图跨方案口径不披露
  + 基线误带部署假设注记 / M4 CSV 无转义）+ 9 Low。M1-M4 全修；
  Low 修了 L1（工具自清）/L3（失败清残留图层）/L4（图表正文色对齐
  token）/L7（空井集保存门）/L8（夹具注释）/L9（评估单次采样）+
  AppContext setProjectData 时序 bug（自审抓到——在 m_projectData 创建前
  调用）+ 导出跳过 retired 旧坐标。
- **轮 2**（只审修复增量）：零 High / 零 Medium；3 Low 全修（attachWellSiting
  幂等缺口、空井失败分支清残留、引号翻倍断言）。
- 既有失败鉴别（stash 基线复测）：tst_projectsvc / tst_ui / tst_wellcomposite_visual /
  tst_singlefactor_perf / tst_startup_trace 在纯 origin/master 基线同样失败
  ——本机环境既有问题，与本方向无关，记 PR body。

## 遗留（记 TODOS/PR body）
- movePlannedWell 是「弃旧建新」（catalog 无 update 面）——方案引用旧 id 失效由
  retired 面吸收，重存方案需用新 id（面板流已覆盖）。
- 诊断图层无 undeclare 面（沿用「覆写空 FeatureCollection、声明保留」约定）。
- lupdate 骨架未重生成（i18n 门禁是源面检查；翻译文件更新走手动 target）。
- review Low 备注未修项：placePlannedAt 错误路径模态框在工具卸载前弹（仅
  catalog 未绑定的错误态可达）；movePlannedWell 弃旧建新的短暂同图中间态；
  工程关闭后 wellsiting 绑定不随 releaseProjectSession 复位（断层集同款惯例）。
- describeField(withBoundaries=false) 仍构造 GeosContext（静态链接代价可忽略）。
