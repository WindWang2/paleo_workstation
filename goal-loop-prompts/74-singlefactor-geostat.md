# Goal-Loop 方向 74：单因素原生算法收口——约束语义全方法消费 + 协克里金接线 + TODOS P2 对账

## 背景（实测事实，勿再勘察；行号为 2026-10-07 master `dc26c0eb`+ 口径）

本方向源自 TODOS.md「P2 — 单因素原生算法后续」（from
goal/single-factor-native, 2026-10-02）。**该条目已部分过时**，
对账后真实剩余面如下（全部核实，勿重查）：

**已落地、条目里还挂着的**（从条目划掉即可）：
- SFPKG 写出：`src/io/sfpkgwriter.{h,cpp}` 297 行 + `tst_io_sfpkg_write`——
  读写已对称。
- 协克里金求解核：`src/algorithms/geostat/cokriging.{h,cpp}` 实现
  `CoKrigingSolver`（primary+secondary 双样本集、γ11/γ22/γ12
  三变差函数模型、maxSecondary 邻域上限）与
  `ConstrainedKrigingSolver` + `WeightGroupConstraint`（带约束
  克里金，即条目所述「带约束 OK」的求解级前身），回归
  `tst_geostat_cokriging` 全绿。
- 变差函数屏障机制：`tst_geostat_variogram_barrier` 存在。

**真实缺口（本方向主体）**：

1. **克里金曲面对约束语义「诚实未消费」**：
   `src/algorithms/singlefactor/krigingsurface.cpp:267-274` 对
   每条启用约束逐条记 issue——`direction_guide_not_used_by_kriging`、
   `soft_boundary_not_used_by_kriging`、`well_cluster_locality_not_
   used_by_kriging`。同一批约束在 IDW 族是**真消费**的：
   `localidw.cpp:145-281` 把方向线解析成 `DirTerm`（CurveKernel
   曲线核 + influence/core/ratio→reduction 局部各向异性张量，
   逐 query/well 门控），`Semantic::ContourStop`/`CartographicDetour`
   也有消费点（~line 383）。即：用户画的方向线在
   `local_direction_idw` 下扭转权重场，在 `local_direction_kriging`
   下被记成「未参与」——同名「local_direction」前缀下语义不对称。
   用户在方向 73 要求约束线绘制「明确为方向线/打断线」——若
   画出来的线在克里金下不生效，UI 再明确也是空转。

2. **协克里金/带约束克里金是「有测试无消费」的死解法器**：
   `CoKrigingSolver`/`ConstrainedKrigingSolver` 除自身测试外
   全仓零引用（已 grep 核实）。协克里金的地质意义是
   「井点硬数据 + 地震/属性软数据联合估值」——缺的是消费面：
   协变量输入契约、`surfaceMethodPacks()` 词表项、
   workflow 接线。`WeightGroupConstraint` 同理未接线。

3. **变差函数逐硬隔断分量拟合**：`krigingsurface` 已有
   `fitted=auto r2=… lags=…` 自动拟合回执（~line 258），但
   硬屏障两侧的变差结构是否分段拟合待 R0 核实
   （`tst_geostat_variogram_barrier` 覆盖面先读）。

**目标**：让约束语义在 IDW 与克里金两族下一致生效或如实降级，
协克里金从死代码变成可选成图方法，TODOS P2 条目收敛到真实
剩余面。

## 环境接线（Linux 本机口径，BUILDING.md「独立 worktree 开发」）

```bash
cd /home/kevin/projects/paleo_workstation
git fetch origin
git worktree add .worktrees/sf-geostat -b goal/sf-geostat-20261007 origin/master
cd .worktrees/sf-geostat
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
[ -d /home/kevin/projects/paleo_workstation/vendor/onnxruntime ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

主回归面：`tst_singlefactor_kriging`、`tst_geostat_*` 全族
（kriging/cokriging/variogram/variogram_barrier/faultpath/
linsolve/neighborhood/sgs*/realarea/workflow）、
`tst_factorworkflow`、`tst_singlefactor_contours` 族、
`tst_io_sfpkg*`、layering 三项。

## 目标形态（建议按序）

1. **R0 勘察定案**：
   - 逐条核实三个 `*_not_used_by_kriging` issue 的可达条件
     （什么 semantic 进 kriging 输入、什么被前置滤掉）。
   - 读 `localidw` DirTerm/CurveKernel 机制与
     `geostat::KrigingSolver` 的各向异性接口（variogram
     azimuth/ratio 是全场单参数——定案方向线是走「局部张量
     场改造距离度量」（同 IDW 思路，在 linsolve 层注入
     各向异性距离）还是「按线分区拟合 variogram」；前者改动
     小且语义同源，后者属逐隔断分量拟合——两者择一或分层
     做，记决策与弃案）。
   - `cokriging` 的样本契约（Sample 结构、secondary 对齐方式）
     与 singlefactor 输入面的差距：协变量从哪来（地震属性
     栅格？第二因子栅格？）——定出最小协变量输入契约。
   - variogram_barrier 测试覆盖面 vs 「逐硬隔断分量拟合」
     的差距：已分段则不重做，只把置信写进 issue 回执。

2. **方向线/软边界进克里金**（本方向核心）：按 R0 定案把
   DirectionGuide/InterpretiveBoundary 消费进 kriging 路径——
   成功消费时三条 `*_not_used_by_kriging` issue 消失（改记
   消费方式回执，如 `direction_guide_applied:N`）；实在不能
   耦合的（如 well_cluster_locality 在去簇语义上需先聚类）
   保留诚实 issue 但写清楚「为什么」。若部分消费（如只在
   各向异性半径内生效），issue 文案如实反映半消费。

3. **协克里金接线**：`surfaceMethodPacks()` 增词表项（label/
   geologicalNote 写实）；workflow 侧接 `CoKrigingSolver`——
   协变量输入源按 R0 定案（首选地震属性栅格或已存在的
   因子栅格资产引用）；缺协变量时如实拒绝（不回落冒充）。
   UI 暴露侧只加协变量选择入口（方向 73 在重组约束页，
   本方向尽量落在请求参数传递层，UI 触碰面最小化——
   若 73 已合入则跟随其面板结构）。

4. **ConstrainedKrigingSolver 接线或销账**：核实它与
   WeightGroupConstraint 的语义 vs 现有 `hard_barrier`
   消费路径——若语义重叠则记录「已被 FaultPathMetric/屏障
   邻域覆盖」并从 TODOS 划掉；若独有（权重组约束），接到
   对应 semantic 的消费点。

5. **TODOS 对账**：「P2 — 单因素原生算法后续」条目按本方向
   落地结果重写——已落地项（sfpkg 写出、协克里金核、克里金
   约束消费）划线归档，仍递延项（时深域转换、监督分类、
   打印排版、完整 Python GUI 嵌入等与本方向无关的）保留
   并标注「与本条目其余项已分流」。

6. **测试**：每个消费点配数值断言回归——方向线扭转克里金
   权重场（同一样本在线两侧权重不对称断言）、软边界衰减、
   协克里金端到端（合成 secondary 栅格 → 估值面与
   CoKrigingSolver 单测口径一致）、缺协变量拒绝路径。
   `tst_geostat_*`/`tst_singlefactor_*` 全绿不可回退。

7. **文档**：`docs/progress/geostat-methods.md` 更新方法矩阵
   （哪些约束 × 哪些方法真消费）；TODOS 条目重写见上；
   MAPPING_WORKBENCH.md 单因素章节的成图方法表同步。

8. **收尾**：工作分支提交，推远端开 PR——按 AGENTS.md 纪律
   **提 PR 不自行合并**；PR 描述列「issue 文案前后对照 +
   数值断言清单 + 遗留项」。

## 通用纪律（方向内全程有效）

- **诚实语义第一**：方法名/issue 文案不得冒充——消费了就写
  消费方式，没消费就写「未参与」，半消费写清边界。沿用
  `method_actual`/`fallback_reason` 先例，禁止静默忽略。
- **与方向 73 的并行边界**：73 重组约束页 UI（区块/文案/
  objectName），本方向动算法与 workflow 接线。交集面是
  「参数如何从 UI 到请求」——若 73 未合入，本方向 UI 侧
  只做最小接线（新增控件命名避撞或走现有 params map），
  等 73 落地后由它吸收；不要预先臆想 73 的结构。
- **数值容差纪律**：克里金/协克里金断言用容差而非逐位
  （平台 BLAS 差异），但 issue 文案断言逐字（UI 契约）。
- **层界**：求解器改动留在 `algorithms/geostat` 与
  `algorithms/singlefactor`；编排接线在 `workflow/`；
  UI 最小触碰。`algorithms` 无 QtWidgets、无 `ui/` include。
- **构建/测试**：`-j8` 上限；`tools/check_layering.py` 三项绿。
- **进度文档**：R0 决策与弃案写 `docs/progress/` 或账本。
