# Goal-Loop 方向 67：单因素域补完——协克里金 / 隔断感知变差函数 / SFPKG 写出 / 策略包 UI

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

TODOS P2「单因素原生算法后续」的剩余项（方向 41 收口后仍缺，
逐项源码验证）：

1. **协克里金/带约束 OK 仍缺**：全仓 `cokriging/CoKriging/
   协克里金` 零命中；克里金只有 OK（`src/algorithms/geostat/
   kriging.h:64` KrigingSolver）+ SGS（sgs.h）。带约束 OK =
  把方向线/软边界耦合进克里金权重。
2. **变差函数不感知硬隔断**：`fitVariogram`（`src/algorithms/
   geostat/variogram.cpp:233`，range 对数扫描 121 档 +
  (nugget,sill) 线性最小二乘）对全样本对 O(n²) 累积、方向
   过滤只有方位角±容差（`variogram.h:15-30`）——隔断感知
   目前只在距离面（`distancetransform.cpp:51-53` barrier
   Dijkstra、`geostat/faultpath.cpp`）。
3. **SFPKG 写侧仍缺**：只有读面（`src/io/sfpkgreader.h:12` +
   `src/io/ziparchive.h:10`「最小内存 ZIP **读**面」）；全仓
   无 sfpkgwriter。ZIP64：读面显式拒收（`ziparchive.h:17`
   「ZIP64（字段是 0xFFFFFFFF）」不支持注记 + `ziparchive.cpp:24`
   kZip64Sentinel）——写出侧 >4GB/条目>65535 需 ZIP64。
4. **外委表曲线统计与因素自动发现 / 制图策略包进 UI**：
   策略参数包词表已落（`src/domain/singlefactorstrategy.*`，
   方向 41）但**未接 UI**——历史制图策略在 UI 里选不到。
5. **既有资产（复用勿重造）**：geostat 核（variogram/
   linsolve/kriging/sgs）、FaultPathMetric（faultpath）、
   localdirection 插值面（krigingsurface 的 method_actual/
   fallback_reason 血缘口径）、外委读面（outsourceworkbook）。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\sf-completion -b goal/sf-completion-20261007 origin/master
cd .worktrees\sf-completion
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。

## 目标形态（建议按序；算法验证用解析解夹具——先例
tst_singlefactor_faultpath 的绕墙折线 hypot 断言）

1. **协克里金核**：`src/algorithms/geostat/cokriging.{h,cpp}`
  ——主变量+协变量交叉变差函数（二级协同模型简化起步：
   交叉变差=主/协按比例的 Markov 近似，口径注释钉死）；
   加边 LU 复用 linsolve；带约束 OK：方向线/软边界作为
   约束行进权重矩阵（不等式约束用软罚——硬约束会破坏 PD，
   口径注释）。
2. **隔断感知变差函数**：fitVariogram 增 barrier 感知档
  ——样本对跨隔断时不进实验变差累积（用 faultpath 的测地
   距离替代欧氏距做滞后距）；与既有非隔断口径双轨并存
  （参数开关），血缘记 method 细分。
3. **SFPKG 写出**：`src/io/sfpkgwriter.{h,cpp}`——镜像读面
   契约（manifest.json + surface.npz + checksum.json，禁
   pickle）；ZIP 写面（stored 起步，deflate 可选）；ZIP64
   写支持（>4GB 场景写 0xFFFFFFFF + ZIP64 extra field——
   读面同步扩，round-trip 测试）。
4. **外委曲线统计与因素自动发现**：outsourceworkbook 读面
   之上加曲线统计（mean/median/min/max + 深度区间）；因素
   自动发现=统计差异显著的相关对列候选（阈值口径注释），
   产出候选清单不自动进图。
5. **策略包 UI**：singlefactorstrategy 词表接单因素页/
   成图工作台的策略选择（下拉 + 参数预览）；所选策略进
   血缘。
6. **测试**：协克里金解析解夹具（两变量已知场重建）；
   隔断变差（隔断两侧样本对不累积的断言）；SFPKG 写→读
   round-trip（含 ZIP64 大夹具）；策略 UI 选择进血缘。

## 通用纪律（方向内全程有效）

- **分层**：算法核归 algorithms（数据层纯数值），读写归 io，
  词表归 domain，编排归 workflow，UI 归 ui；`check_layering.py
  --strict` 绿。
- **诚实面**：协克里金近似口径（Markov 假设）注释+文档如实；
   隔断感知是双轨不是替换；自动发现是候选不是结论；回落
   method_actual/fallback_reason 血缘口径沿用。
- **数值纪律**：解析解夹具先行；禁绝对毫秒断言。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：Markov 近似等口径自行定案记 ledger（宁可
  保守不过声称）。
- **vendor 纪律**：不引第三方地统库；zip 写面用 zlib（既有）。
- **ledger**：`.goal-loop-ledger-sf-completion.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/数值正确性/契约 round-trip/诚实面/i18n 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 协克里金：两变量合成场（已知交叉关系）重建误差收敛
  （容差断言）；退化单变量=OK 结果（一致性断言）；带约束
   OK 的约束行生效（权重响应断言）。
2. 隔断变差：隔断两侧样本对不进累积（计数断言）；开关关闭
   时与既有 fitVariogram 逐位一致（对拍）。
3. SFPKG：写→读 round-trip（数值逐位一致）；ZIP64 大夹具
  （>4GB 或条目>65535 构造）round-trip 过；坏 checksum
   拒收。
4. 外委统计：夹具统计值与直算一致；因素候选清单可复现
  （同输入同输出）。
5. 策略 UI：选择后血缘含策略 id；未知 id 不回退（沿用词表
   口径）。
6. 全量 ctest 对照 R0 红集合 diff 为空；既有 geostat/
   singlefactor 测试全绿（方向 18/41 基线不回归）。
