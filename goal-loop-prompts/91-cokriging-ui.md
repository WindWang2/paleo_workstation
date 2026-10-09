# Goal-Loop 方向 91：#310 处置与方向 84 增量收口——UI 协变量面 + 隔断感知拟合落地

## 背景（实测事实，勿再勘察；行号为 2026-10-09 master `32851a1e` 口径）

方向 84 出现了**同任务书平行实现**：#308（已并入 master）与
#310（OPEN，`origin/goal/sf-geostat-20261009`，基点 f31ee461
不含 82ea9cd9）都执行 74 号任务书。四轮盘点裁决：

- **#308 已为准**（谁先合谁为准）：MLA 度量装配
  （`src/algorithms/singlefactor/localidw.cpp:344-349`）、
  `direction_guide_applied:N`/`soft_boundary_applied:N` 回执
  （`krigingsurface.cpp:318-322`）、ConstrainedKrigingSolver
  2D 销账、协克里金算法核+workflow 全链接线
  （`constraintfactorjobs_geostat.cpp:229-242,519-537,614-637`）。
- **#310 独有增量（#308 的两个已知缺口）**：
  1. **UI 协变量面**：master 的 `covariateLayerId` 无任何
     UI 写入点（`rg covariateLayerId src/ui` = 0）——用户在
     方法下拉选了协克里金（`singlefactorstrategy.cpp:82` 注册
     进 surfaceMethodPacks）却只能走 workflow 诚实报错路径，
     **已暴露的死选项**。#310 有 `factorCovariateCombo`
     （仅 cokriging 时可见 + ρ 输入）。
  2. **隔断感知拟合**：geostat 核 barrier 档在
    （`variogram.h:27,49,55`）但 singlefactor 侧
     `resolveVariogram` 不传 barriers（`krigingsurface.cpp:67`
     签名无 barriers 参数）——跨隔断样本对仍进同一结构
     拟合。#310 的 D4 补这个。
- **#310 的深度冲突**：其 PairMetricWarp 方案与 #308 的 MLA
  装配在 `krigingsurface.{h,cpp}`/`localidw.cpp`/
  `cokriging.{h,cpp}`/`kriging.{h,cpp}` 五文件语义互斥；
  其 TODOS 版本会回退 #302 已交付注记（方向 80 键名动态化）
  并删除方向 80 拆分候选清单（TODOS.md:21-31）。

**校正（对照 origin/master `753c2768` 与 OPEN 的 #316；上面
「隔断感知拟合」不要读成一条链）**：

- 只改 `singlefactor::resolveVariogram`
  （`src/algorithms/singlefactor/krigingsurface.*`）影响的是
  `evaluateLocalKriging` / `method=local_direction_kriging`。
  普通克里金、协克里金、SGS 走
  `ConstraintWorkflow::generateGeostatFactor` →
  `computeGeostatJob`（`constraintfactorjobs_geostat.cpp`
  约 432/434/453 行的 `experimentalVariogram` **不传**
  barriers）。`GeostatJob`（`src/workflow/workflows.h`）没有
  约束几何字段，约束线到不了这条拟合。
- #316（`goal/cokriging-ui-20261010`，撰写本校正时仍 OPEN、
  **未并入 master**）只落地了 `resolveVariogram` 这一窄增量，
  且没有改 `constraintfactorjobs_geostat.cpp`。其 PR body 的
  Low 已写明：kriging/cokriging/SGS 的自动拟合仍用欧氏滞后距。
  **不要把 #316 写成已经做了 geostat 拟合路径。** 执行时若
  #316 已合入，窄增量不要重做；未合入则在 #308 基线上重做窄
  增量。无论 #316 是否已合，窄增量都不等于三条 geostat 方法
  已隔断。
- `src/domain/singlefactorstrategy.cpp` 的
  `SurfaceMethodPack::geologicalNote` 仍有用户可见假句
  （方法下拉 tooltip）。只改 TODOS.md 消不掉。

**本方向 = 丢弃 #310 的重复实现，在 #308 基线上重做两个真
增量，并按上面两条拟合链拆开验收**（#308 为准；#316 的窄
增量不算 geostat 路径已完成）。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\cokriging-ui -b goal/cokriging-ui-20261010 origin/master
cd .worktrees\cokriging-ui
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152（方向 72 统一链）。

## 目标形态（建议按序）

1. **R0 对账**：逐文件对拍 #308（master）与 #310 两套实现
   的差异面（krigingsurface/localidw/cokriging/kriging 四件
   + TODOS），定「保留什么/重做什么」清单记 ledger。
2. **UI 协变量面（重做 #310 增量①）**：constraintpage 参数
   构造段（#308 未触碰，`constraintpage.cpp:559-634` 区间）
   增协变量图层下拉 + ρ 输入——仅 method==cokriging 时
   可见启用（objectName 稳定命名先例：wellFactorSection）；
   `covariateLayerId` 进 params 传到 workflow（现状 workflow
   侧契约已就绪 `constraintfactorjobs_geostat.cpp:229-242`）；
   无可选协变量图层时下拉空态 + 提示（不冒充可选）。
3. **隔断感知拟合按两条链做，禁止用窄增量冒充全方法**：
   - **窄增量（`local_direction_kriging`）**：`resolveVariogram`
     增 barriers，自动拟合把硬屏障传进
     `experimentalVariogram` 的测地滞后距档；血缘记
     `barrierAware:true`；关开关或无屏障时与 #308 欧氏拟合
     逐位一致。这是目标要求的算法改动，见下方分层豁免。
     #316 若已合入则不要重做。
   - **宽增量（#316 没做）**：普通克里金、协克里金、SGS 必须
     把屏障/约束几何带进 `GeostatJob`，并在
     `constraintfactorjobs_geostat.cpp` 的拟合处传给
     `experimentalVariogram`。不做这条，就在 ledger、TODOS、
     PR body 里把它标成 **#316 未做的剩余工作**，禁止写
     「resolveVariogram 已接线，故 kriging/cokriging/sgs
     已隔断」。`tst_geostat_variogram_barrier` 零改动通过只
     证明 geostat **核**，不证明工作流拟合已接屏障。
4. **TODOS 与用户可见注记一起对账**（只改 TODOS.md 不够）：
   - `TODOS.md:109`「地统 Kriging/SGS 的逐线屏障语义」按两条
     链拆开。`local_direction_kriging` 的方向线/软边界权重是
     #308 已消费的（MLA + 软边界距离膨胀，回执
     `direction_guide_applied:N` / `soft_boundary_applied:N`）。
     硬屏障进变差拟合：只有对应路径真传了 barriers 才能标已
     消费。不要照抄 #316 把「地统 Kriging」整句打勾——那只
     覆盖了 `resolveVariogram`。SGS 在 geostat 拟合路径接上
     屏障之前保持递延。
   - **必须改** `SurfaceMethodPack::geologicalNote`
     （`src/domain/singlefactorstrategy.cpp`；
     `constraintpage.cpp` 把它设成方法下拉 tooltip）。
     origin/master `753c2768` 上，`local_direction_kriging` 这句与 #308 矛盾；`kriging` 那句只描述 geostat 核路径，不能拿来概括 #308 已消费的本地方向克里金：
     - `local_direction_kriging`：「方向线与软边界不参与克里金权重并逐条列入 issues。」
       假。#308 在 `localidw.cpp` 克里金分支用 MLA 消费方向线、
       用跨界距离膨胀消费软边界；`krigingsurface.cpp` 写的是
       applied 回执，不是「不参与」issue。注记改成与回执一致。
     - `kriging`：「变差函数自动/显式拟合 + 普通克里金局部邻域求解（geostat 核，逐线屏障/方向参数不参与）。」
       这句只许描述 `generateGeostatFactor` 那条 geostat 核的
       真实状态。宽增量做完了就改成已消费并写清消费的是硬屏障
       拟合还是方向线/软边界权重；没做就保留「不参与」，但不得
       让读者以为 `local_direction_kriging` 也是这句。
   - 方向 80 键名注记与拆分候选清单保持
     （不回退 #302 事实——#310 之鉴）。
5. **#310 遗产摘取**：其 PR/review 中与 #308 无冲突的发现
  （crossCorrelation 进参数指纹、covariate CRS transform 用
   processing context 等）逐条评估转入实现或 TODOS。
6. **测试**：UI 协变量（offscreen——下拉可见性/params 断言）；
   隔断拟合按目标 3 两条链分别给断言（窄增量不得充当
   kriging/cokriging/sgs 的验收）；#308
   既有 16 槽 tst_singlefactor_kriging 零改动通过。

## 通用纪律（方向内全程有效）

- **分层**：UI 归 ui/pages，参数契约归 domain。
  **豁免、且本方向要求改的算法面**：
  `src/algorithms/singlefactor/krigingsurface.{h,cpp}` 的
  `resolveVariogram`（增 barriers / 隔断感知拟合）。禁止用
  「不动 algorithms 核」挡掉这次改动。除此之外不改 #308 已
  验收的 MLA、回执文案、协克里金求解核。宽增量允许改
  `GeostatJob` 与 `constraintfactorjobs_geostat.cpp` 的拟合
  传参，以及 `singlefactorstrategy.cpp` 的 geologicalNote。
  `check_layering.py --strict` 绿。
- **行为红线**：#308 已验收语义（MLA/回执/销账）零变更；
  不引入 #310 的 PairMetricWarp（与 MLA 互斥）。窄增量不得
  冒充已覆盖 kriging/cokriging/sgs。
- **诚实面**：协变量缺失/CRS 不匹配如实报错（workflow 侧
  已有纪律沿用）；UI 空态不冒充。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行。
- **无人值守**：重做取舍自行定案记 ledger（每件给「摘取/
  放弃」判据）。
- **ledger**：`.goal-loop-ledger-cokriging-ui.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/#308 语义保留/增量正确性/TODOS 同步/i18n 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. UI 死选项闭环：cokriging 选中时协变量下拉可见启用、
   `covariateLayerId` 传抵 workflow（params 断言）；非
   cokriging 时隐藏；空图层集空态如实。
2. 隔断拟合按链给证据：`local_direction_kriging` /
   `resolveVariogram` 有跨隔断对剔除计数、`barrierAware`
   血缘、关开关与 #308 逐位一致。kriging/cokriging/sgs 要么
   有 `GeostatJob` 携带屏障且
   `constraintfactorjobs_geostat.cpp` 拟合传 barriers 的
   断言，要么 ledger/TODOS/PR body 明确这是 #316 未做的剩余
   工作。禁止只拿 `resolveVariogram` 测试充当三条方法的验收。
3. TODOS:109 按两条链拆开，不把 geostat 克里金/SGS 整句打成
   已消费，除非宽增量真落地；`geologicalNote` 两句与 #308
   事实一致（diff 证据）；方向 80 注记与候选清单原样
   （零回退）。
4. #310 处置：摘取/放弃清单入 ledger（每件判据）；其
   TODOS 回退零发生。
5. 回归：#308 全部既有测试（tst_singlefactor_kriging 16 槽/
   tst_geostat_cokriging 9 槽）零改动通过；全量 ctest 对照
   R0 红集合 diff 为空。
6. layering 三档绿；check_i18n 绿。
