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

**本方向 = 丢弃 #310 的重复实现，摘取其两个真增量在 #308
基线上重做**（用户/维护者裁决口径：#308 为准，增量落地）。

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
3. **隔断感知拟合接线（重做 #310 增量②）**：`resolveVariogram`
   增 barriers 参数（测地滞后距档），singlefactor 链把方向
   67 已落地的 barrier 核接进来；血缘记 `barrierAware:true`；
   与非隔断档双轨（开关），既有 tst_geostat_variogram_barrier
   零改动通过。
4. **TODOS 对账修正**：`TODOS.md:109`「Kriging/SGS 的逐线
   屏障语义」拆分——克里金半句更新为已消费（方向 74/84），
   SGS 半句保留递延；方向 80 键名注记与拆分候选清单保持
  （不回退 #302 事实——#310 之鉴）。
5. **#310 遗产摘取**：其 PR/review 中与 #308 无冲突的发现
  （crossCorrelation 进参数指纹、covariate CRS transform 用
   processing context 等）逐条评估转入实现或 TODOS。
6. **测试**：UI 协变量（offscreen——下拉可见性/params 断言）；
   隔断拟合（合成隔断场景，跨隔断对剔除计数断言）；#308
   既有 16 槽 tst_singlefactor_kriging 零改动通过。

## 通用纪律（方向内全程有效）

- **分层**：UI 归 ui/pages，参数契约归 domain，执行已就绪
  不动 algorithms 核（#308 交付）；`check_layering.py --strict` 绿。
- **行为红线**：#308 已验收语义（MLA/回执/销账）零变更；
  增量面只加不改；#310 的 PairMetricWarp 不引入（与 MLA
  互斥，避免双方案并存）。
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
2. 隔断拟合：合成隔断场景跨隔断对剔除（计数断言）；
   `barrierAware` 血缘标记；开关关闭时与 #308 逐位一致
  （对拍）。
3. TODOS：`:109` 行拆分修正（克里金已消费/SGS 保留递延）；
   方向 80 注记与候选清单原样（零回退——diff 证据）。
4. #310 处置：摘取/放弃清单入 ledger（每件判据）；其
   TODOS 回退零发生。
5. 回归：#308 全部既有测试（tst_singlefactor_kriging 16 槽/
   tst_geostat_cokriging 9 槽）零改动通过；全量 ctest 对照
   R0 红集合 diff 为空。
6. layering 三档绿；check_i18n 绿。
