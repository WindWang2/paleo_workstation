# Goal-Loop 方向 17：断层立体化——断棒成面与断距量算（Fault Surface Assembly）

## 背景（实测事实，勿再勘察）

- **断层解释框架（#7）已落地**：`src/domain/faultset.h`——`FaultSet/Fault/FaultStick/FaultSectionRef/FaultHorizonCut`；棒按剖面追加（`addStick` 分配 `s-N`）、`sticksForSection` 按 `matchKey` 取剖面断棒、`FaultHorizonCut` 记层位-断层切割对（上下盘距离）；持久化 `src/metadata/faultsetstore.*`（project.sqlite 共库，user_version 门）；UI 侧剖面拾取 + `src/ui/faults/faultmanagerpanel`；测试 `tst_faultset/tst_faultsetstore/tst_faultinterp/tst_faultsectionui`。
- **缺的是断面**：断棒是逐剖面 2D 折线，Fault 没有一个「面」实体——无法回答「断层在任意 X/Y 的空间展布」「断距沿走向/倾向的变化」「断面与层位面的三维交切」。编图/属性建模要的「断层」本质上是面不是棒集。
- **下游已在用阻断语义**：`#13` 属性建模（`src/algorithms/stratgrid` + `propertymodelworkflow`）实现了断层两侧不互作插值邻域的阻断，但只能按棒/切割多边形做平面投影阻断——有了断面才能升到体域阻断。
- **3D 视口已有体渲染通路**（`src/ui/seismic3d/*`，GL 3.3 Core，切片/TF/扫掠），面渲染是天然挂载位。
- 棒-面关联排序、stick 深度域（TWT/深度）一致性问题在设计上有现成锚点：`FaultSectionRef` 已携带剖面身份（kind+lineNo/volume 键），`FaultHorizonCut` 已记断距语义。
- 真工区 `/home/kevin/projects/paleo_project/data/project_area`（env `PALEO_REAL_PROJECT_AREA`）有 966MB SEG-Y + 层位/断层成果可实测。
- 纪律：读 DESIGN.md 再做 UI；每 `src/` 文件三层标记；`add_paleo_test`；`check_layering --strict` 绿；oracle 不接受「应该没问题」。

## 目标形态

**断层棒 → 三角网断面 → 量算/渲染/下游阻断**，最小闭环四段：

1. **断面构建核**（`src/algorithms/faultsurface`，新模块先 `scripts/new_module.sh` 登记）：
   `buildFaultSurface(fault)` 把同一断层的多剖面断棒按剖面位置排序成面——剖面间三角剖分（相邻棒点云配准→条带三角网），输出 `FaultSurface{ vertices, triangles, stickRefs }`。边界情形如实处理：单剖面断层 → 退化为「向上下半延」的有界面片或如实拒绝（选其一并写进语义决策）；棒走向突变/分叉（Y 型）先不支持、检测后显式失败。
2. **量算**：断面上任意点/沿走向剖线的走向/倾向/倾角；`FaultHorizonCut` 升级为断面口径——层位面与断面相交线 + 断距（heave/throw）沿交线的分布曲线。
3. **持久化**：FaultSet schema 演进（faultsetstore user_version 步进 + 迁移：`docs/SCHEMA_MIGRATION.md` 流程）——Fault 挂 `surface`（mesh 本体可落 catalog DERIVED 资产或存 store blob，看体量定，决策写账本）。
4. **可视化与消费**：3D 视口断面渲染（半透明面 + 断棒叠加校验贴合度）；剖面画布断面-剖面交线显示模式（非拾取剖面上看断面穿过的位置）；`stratgrid` 充填阻断从平面投影升级为断面体域查询（如现接口允许最小改动则做，否则登记递延）。

## Oracle 验收（全部须实测通过并记账本）

1. **成面几何**（纯数值，`tst_faultsurface`）：
   - 合成断层：3 条平行剖面各 1 条直线断棒（倾角已知）→ 断面法向/倾角数值断言（±0.5°）；顶点全落在棒上或剖面间插值面上；三角网无退化三角形（面积>0）、无悬边（每条边 ≤2 个三角形，开边界边恰好 1）。
   - 棒点数不一致/乱序剖面 → 仍成面且 stick 归属正确；单棒断层按选定语义走（退化面片或显式拒绝，断言一致）。
2. **断距量算**：合成平移断层（已知 heave/throw 常数）→ 任意层位交线量出的断距 ≈ 真值（绝对误差断言）；沿交线分布曲线长度/值域断言。
3. **持久化往返**：FaultSet（含断面）store 写→重开读，棒/切割/断面 mesh 字段级相等；schema 迁移路径从旧库走通（旧库无 surface 列 → 迁移后 surface 空但棒/切割无损）。
4. **编排与资产**：`FaultSurfaceWorkflow`（功能层）串「选断层→成面→校验→登记」；断面 mesh 落 DERIVED 资产时 parent = 源 FaultSet 版本（按 #106 produce-then-commit 先例）。
5. **可视化闭环**：3D 视口断面渲染 smoke（渲染调用可达、相机包含面 bbox）；剖面交线模式在非拾取剖面出正确交点位置（数值断言）。
6. **性能**：100 剖面 × 200 点/棒 合成断层 < 3s 成面、mesh 内存有界；比率门记账。
7. ledger 全账 + `docs/progress/fault-surface.md`（单棒断层语义、深度域一致性口径、mesh 存储位置决策、Y 型/分叉递延清单）。

## 勘察指引

- `src/domain/faultset.{h,cpp}`（Fault/FaultStick/FaultSectionRef/FaultHorizonCut 结构与本域模型）、`src/metadata/faultsetstore.{h,cpp}`（schema 与 user_version 步进先例）
- `src/workflow/faultinterpretationcontroller.{h,cpp}`（断层编排先例）、`src/ui/faults/faultmanagerpanel.cpp`（面板接点）
- `src/algorithms/`（新算法模块形态参考 `mincurvature`/`stratgrid`——纯数值无 Qt）
- `src/ui/seismic3d/`（体渲染挂载点）、`src/ui/seismicsection/seismicsectioncanvas*`（剖面叠加层先例——层位/断层棒绘制就在那里）
- `src/algorithms/stratgrid` 阻断接口（如可最小接入则升级，否则递延写清）
- `docs/progress/fault-interpretation.md`、`property-modeling.md`（前两批语义决策，保持口径一致）

## 禁区

- 不做 Y 型/分叉/多分支断层成面——检测到分叉棒拓扑显式失败并递延。
- 不做断面自动生长/从断层多边形反插断面——输入只有断棒。
- 不改 FaultStick 拾取交互与 FaultHorizonCut 既有语义（只升级量算口径为断面）。
- 不碰 `src/catalog` 持久层（goal/catalog-sqlite 在飞）与 `src/services` 井读面（goal/well-logset 在飞）——避免同文件面冲突。
- 不引第三方网格库（CGAL/meshlab）；三角剖分自研条带算法即可。
- 棒若混时深域（有的剖面 TWT 有的 depth）→ 成面前显式校验拒绝，不静默混域。

## 迭代协议

- **轮0**：勘察定案——FaultStick 的坐标语义（剖面内 x=道位 y=时间/深度？）、剖面空间位置可得性（inline/xline→XY 映射在 faultset 还是投影服务）、FaultHorizonCut 断距字段口径；接口签名进 ledger。
- **轮1**：`faultsurface` 成面核 + `tst_faultsurface`（Oracle 1 全断言）。
- **轮2**：断距量算 + 层位交线 + `tst_faultsurface` 扩（Oracle 2）。
- **轮3**：store schema 演进 + 迁移 + `tst_faultsetstore`（Oracle 3）。
- **轮4**：`FaultSurfaceWorkflow` + DERIVED 登记 + `tst_faultsurfaceworkflow`（Oracle 4，含失败诚实）。
- **轮5**：3D 渲染挂载 + 剖面交线模式 + stratgrid 阻断升级或递延 + `tst_faultsectionui` 级回归。
- **轮6**：真工区实测 + 性能记账 + docs/progress 收口。

## 交付协议（必守）

1. **新建 worktree 分支开发**，禁止在主 checkout 写代码：
   `git worktree add .worktrees/fault-surface -b goal/fault-surface-<date> origin/master`
2. 每轮原子提交，ledger 同步记账。
3. **完成后必须先仔细 review 再开 PR**：
   - `git diff origin/master...HEAD` 全量自审：无调试残留/死代码；层标记齐；`check_layering --strict` 绿；
   - vendor 前缀全量构建零新警告，ctest 全绿；
   - Oracle 每条有命令+输出摘要证据；
   - 发现问题先修再验直到干净。
4. 过门禁后 `git push` + `gh pr create`，不等 CI 不等人。
