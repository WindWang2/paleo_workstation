# 方向 91 ledger：#310 处置与协变量 / 隔断拟合增量

基线是 origin/master `753c2768`（含 #308 及之后的合入）。#310 尖端是
`origin/goal/sf-geostat-20261009` `1db03ec5`，基点 f31ee461，不含 #308。

## R0 对拍

| 面 | 裁决 | 判据 |
|---|---|---|
| MLA 度量、`direction_guide_applied` / `soft_boundary_applied`、ConstrainedKrigingSolver 销账、协克里金核与 workflow | 保留 #308，不改 | 谁先合谁为准；#310 的 PairMetricWarp 与这套语义互斥 |
| `krigingsurface` / `localidw` / `cokriging` / `kriging` 的权重公式 | 放弃 #310 | 五文件与 #308 互斥，引入会变成两套克里金 |
| TODOS 方向 80 键名注记与拆分候选清单 | 放弃 #310 的版本 | 会回退 #302 已交付事实 |
| UI `factorCovariateCombo` + ρ | 摘取并在 #308 上重做 | master 上 `covariateLayerId` 没有 UI 写入点，协克里金是已暴露的死选项 |
| `resolveVariogram` 隔断感知拟合 | 摘取并在 #308 上重做 | geostat 核已有 `VariogramBarriers`，singlefactor 自动拟合没把硬屏障传进去 |
| `crossCorrelation` 进参数 | 摘取为 UI 写入 | workflow 已读该键并写入 QC `cross_correlation`，缺的是页面把 ρ 放进 params |
| 协变量采样的 CRS 变换 | 放弃实现，记 TODOS | 要改 #308 已验收的 `sample(x, y)` 路径；没有证明现网井栅格 CRS 不一致的夹具。同 CRS 时变换应是恒等，但改采样没有对拍测试 |

## 落地

- 协变量下拉与 ρ 只在 `method==cokriging` 时可见。空图层集只有「（无——将拒绝协克里金请求）」，参数仍带空 `covariateLayerId`。
- `variogramBarrierAware` 默认开。自动拟合且样本不超过 512 时，启用的硬屏障走测地滞后距。关开关或没有硬屏障时不传 barriers，与旧欧氏调用逐位一致。显式 `range>0` 不拟合，只在 issues 里说明。
- 血缘 issues 增加 `barrierAware:true`。不改 `direction_guide_applied` / `soft_boundary_applied` 文案。
