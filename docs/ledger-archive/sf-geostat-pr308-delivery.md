# PR #308 交付说明（单因素原生算法收口——方向 74）

> 本说明由方向 100（2026-10-10）依据 **squash 提交 `82ea9cd9` 自身的提交说明
> 与 diff** 重建。#308 合入时未携带 goal-loop 账本（squash 快照无账本文件），
> 本文件补的是交付说明，**不是** review 账本——PR #310 的账本（见
> sf-geostat-pr310.md）是平行实现的记录，其第 5/6 轮 review 计数
> （1H/5M/7L、0H/1M/5L）**不属于 #308**，不在此引用。

## 合并事实

- 提交：`82ea9cd9e123490c0c3f7e41f246ad30a85b1760`
  `feat(sf-geostat): 单因素原生算法收口——约束语义全方法消费 + 协克里金接线
  + TODOS P2 对账 (方向 74) (#308)`
- PR #308，方向 74（74 号任务书）；合入 master，成为该任务书唯一落地的
  实现（#310 平行分支未合入）。
- 规模：14 文件，+1,654 / −214 行。

## 交付面（按提交说明原文归纳）

1. **约束语义消费**：通过 Moving Local Anisotropy 黎曼度量张量消费走向线，
   通过连续软边界距离膨胀消费解释软边界，消除跨界台阶与折痕，生成
   `direction_guide_applied` / `soft_boundary_applied` 真实回执
   （`src/algorithms/singlefactor/localidw.cpp` +284、
   `krigingsurface.cpp` +67）。
2. **协克里金求解核与工作流**：`ordinaryCoKriging` 全场网格求解
   （`src/algorithms/geostat/cokriging.{h,cpp}` 新增 233 行）；
   `surfaceMethodPacks()` 注册词表项（`domain/singlefactorstrategy.*`）；
   工作流接线支持次级协变量图层契约 `covariateLayerId`
   （`workflow/constraintfactorjobs_geostat.cpp` +184、
   `constraintfactorjobs_factor.cpp`）；缺失协变量诚实报错拒绝；
   输出估值与方差双栅格。
3. **解法器销账**：`ConstrainedKrigingSolver` 从 2D 曲面流水线正式退役并对账。
4. **文档与对账**：TODOS.md P2 条目 7 落地项归档、3 递延项标明去向
   （+53 行）；`docs/progress/geostat-methods.md` 与
   `docs/workflows/MAPPING_WORKBENCH.md` 更新（后者 diffstat ±57 行——
   #310 账本「MAPPING_WORKBENCH.md 不存在于 master」的勘察失实句，
   见档案注记）。
5. **回归与层界**（提交说明原文）：40/40 测试全绿，
   `check_layering.py --strict` 保持 0 违规。

## 测试面（diff 内新增）

- `tests/tst_singlefactor_kriging.cpp`（方向 73 期既有文件扩容 +475）
- `tests/tst_geostat_cokriging.cpp`（+81）
- `tests/tst_factorworkflow.cpp`（+137）

「40/40」是提交说明记录的当时口径。增量面（隔断感知变差拟合
`variogramBarrierAware`、协克里金协变量 UI）由后续方向 91（#316）承接，
TODOS「地统 Kriging 的逐线屏障语义」条目已按 74+91 双链对账；
SGS 逐线屏障语义仍递延（该条目现状：现方法不消费约束线，如实呈现）。
