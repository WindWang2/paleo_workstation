# Goal-Loop 方向 69：连井剖面 TVD 域 + 解释岩性道（方向 38 修订重发）

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

原方向 38（wellsection-tvd）从未执行，但其背景已部分过时——
本方向为修订重发（勿执行原任务书）：

**原四目标的现状**：
- 目标 2（真实井距）**已实现**：`SpacingMode { Equal,
  Proportional }`（`src/domain/wellsection.h:101`）+
  `gapWidthsFor()`（`:104`），面板有「井距：等距 / 按井口
  距离比例」切换（`src/ui/wellsection/wellsectionpanel.cpp:220-222`，
  提交 42b9a57 已在 HEAD）——**勿重做**。
- 数据源就绪：`WellLogSet::readCurveTvd`（`src/services/
  welllogset.h:86`，goal/well-trajectory 轮 3）——TVD 读取面
   已备。
- **目标 1（TVD 深度域）未做**：`DatumMode` 只有
  `{ Depth, Elevation, Flatten }`（`src/domain/wellsection.h:154-157`
  区间），无 TVD；`src/ui/wellsection/` 全目录 tvd 零命中。
- **目标 3（解释岩性道）未做**：仍是 GR 截断砂/泥二分
  `inferSandShale`（`src/domain/wellsection.cpp:499`），无
  解释岩性数据源接入。
- 目标 4（模板随工程走）维持原递延口径（本方向不做）。

**解释岩性数据源现状**：方向 27/12 落的 welllogfacies
（`src/ai/wellfaciesservice.*` 测井相预测）与 catalog 的相
分类面（方向 14/46 交会相/监督分类）都可产出逐深度岩性/
相标签——但剖面的岩性道没有消费口（rg 验证 wellsection
对 facies 零引用）。缺的是「解释岩性 provider」接线。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\wellsection-tvd -b goal/wellsection-tvd-20261007 origin/master
cd .worktrees\wellsection-tvd
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。主回归面：tst_wellsection 族 + wellsection
相关 UI 测试。

## 目标形态（建议按序）

1. **TVD 深度域**：`DatumMode` 增 `Tvd`（domain 词表扩展）；
   曲线/分层/时深表读取切 `readCurveTvd`；无测斜数据的井
   **如实标注**「TVD 不可用（无测斜）」并按 MD 绘制 + 顶部
   标记（不静默冒充）；深度轴/标尺/道头单位同步 TVD(m)。
2. **域切换语义**：Depth↔TVD 切换保持井序/对比基准选择；
   深度域进剖面状态（导出/截图携带口径标签——图签先例）。
3. **解释岩性 provider**：`src/domain/wellsection` 增解释
   岩性通道（provider 接口：逐深度岩性/相标签 + 来源标注）；
   首个 provider 接 welllogfacies 预测结果（catalog DERIVED
   面）；GR 截断推断降级为 fallback（来源标签区分
   「解释/推断」）。
4. **岩性道渲染**：解释岩性用地质符号/花纹渲染（方向 31
   symbols 库消费）；图例随来源标注；混合来源（部分井有
   解释、部分推断）如实分色。
5. **测试**：TVD 域合成数据（已知测斜的 TVD 曲线对比 MD
   偏移）；无测斜降级标注；provider 注入的岩性道渲染
  （offscreen 断言道内容与注入标签一致）；域切换状态保持。
6. **文档**：方向 38 原任务书头部加「已修订重发为方向 69」
   注记（防误发）。

## 通用纪律（方向内全程有效）

- **分层**：深度域/岩性通道归 domain，读取编排归 workflow/
  services，道渲染归 ui/wellsection；`check_layering.py
  --strict` 绿。
- **诚实面**：TVD 不可用如实标注（不冒充）；岩性来源标签
   区分解释/推断；缺数据井不画假道。
- **DESIGN.md**：岩性道配色/符号视觉先读 DESIGN.md 与方向
   31 符号库口径。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：provider 接口形态自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-wellsection-tvd.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/TVD 数学正确性/降级诚实/渲染一致/文档同步 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. TVD 域：合成测斜井的剖面深度值与直算 TVD 一致（容差
   断言）；深度轴/单位/图签口径正确。
2. 降级：无测斜井按 MD 绘制 + 「TVD 不可用」标注可见
  （offscreen 断言）；不 crash 不静默。
3. 岩性道：注入解释标签的井渲染对应符号/颜色（断言）；
   来源标签区分解释/推断；混合井列表如实分色。
4. 切换：Depth↔TVD 切换井序/基准保持（状态断言）。
5. 回归：tst_wellsection 族零改动通过；既有 MD 域行为
   不变（对拍）。
6. 全量 ctest 对照 R0 红集合 diff 为空；方向 38 任务书已加
   修订注记。
