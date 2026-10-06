# Goal-Loop 方向 66：wellcomposite 家族拆分——track 1,875 + panel 1,640 + canvas 1,549 三文件解体

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

`src/ui/wellcomposite/` 测井综合图家族是次级巨兽聚簇（合计
5,064 行三文件）：

- `wellcompositetrack.cpp` **1,875 行**——综合图道：D1.11 公共
  道头三行区自适应截断、曲线充填、道配置。
- `wellcompositepanel.cpp` **1,640 行**——面板编排：井选择、
  道布局、深度工具、编辑会话。
- `wellcompositecanvas.cpp` **1,549 行**——画布渲染：分层柱、
  岩性道、曲线绘制。

**拆分先例**：方向 56（datalist 3,839→938 + 7 域 TU +
datalistops.h 辅助头）为同层同模式范本；方向 55/65 为相邻
家族先例。

**行为红线（wellcomposite 特有）**：道头自适应截断语义
（D1.11）、曲线配置对话框与 track 的往返契约、编辑会话
（editsession.cpp 同目录协作）、深度工具联动（depthtools）；
消费面：wellsection 剖面引用综合图数据（`src/domain/
wellcompositemodel.h` 契约）——**domain 契约不动**。

**协调面**：与方向 65（seismicsection 家族）同模式不同目录，
可并行；本目录 tst_wellcomposite_* 族是主回归面（含
tst_wellcomposite_depth/tst_wellcomposite_shell 等）。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\composite-split -b goal/composite-split-20261007 origin/master
cd .worktrees\composite-split
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。

## 目标形态（建议按序）

1. **R0 结构勘察**：三文件各自按职责盘点（track：道头/曲线
  充填/道配置；panel：编排/选择/会话；canvas：分层柱/岩性/
  曲线绘制）画段位图；与同目录协作文件（editsession/
   depthtools/curveconfigdialog/stratassignment）的边界核对；
   切分线记 ledger 定案。
2. **公共 API 冻结**：三头公共区 diff 对拍零变更；domain 契约
  （wellcompositemodel.h）零改动；消费方零改动。
3. **track 拆分**：道头渲染/曲线充填/道配置三域拆 TU；道头
   截断语义单独成节注释。
4. **panel 拆分**：编排/井选择/道布局/编辑会话接线分 TU。
5. **canvas 拆分**：分层柱/岩性道/曲线绘制分 TU；渲染资源
   缓存语义注释保留。
6. **收口**：三主文件各 ≤800 行；新 TU 各 ≤900 行；CMake
   更新；include 纪律。

## 通用纪律（方向内全程有效）

- **分层**：全部留在 `src/ui/wellcomposite/`（视图层）；新
  文件头三行 `// 层：视图`；`check_layering.py --strict` 绿。
- **行为保留红线**：道头截断/配置往返/编辑会话/深度联动语义
  逐条不变；「顺手改进」违规记 TODOS。
- **测试口径**：tst_wellcomposite_* 族每批全绿；全量构建后
  再 ctest。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：切分线自行定案记 ledger。
- **性能断言**：综合图渲染性能门不回退（比率门对照 R0）。
- **ledger**：`.goal-loop-ledger-composite-split.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （API 等价/道头语义/会话契约/TU 边界/无死代码 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 类零变更：三头公共区 diff 零变更；wellcompositemodel.h
   零改动；消费方零改动（rg 证据）。
2. 体量：三主文件各 ≤800 行；新 TU 各 ≤900 行（wc 证据入
   ledger）。
3. 语义保留：tst_wellcomposite_* 族零改动通过×2 遍；道头
   截断/配置往返回归用例原样绿。
4. 性能门：综合图渲染对照 R0 比率无 >10% 劣化。
5. 全量 ctest 对照 R0 红集合 diff 为空；layering 三档绿；
   构建无新警告；git diff --check 干净。
