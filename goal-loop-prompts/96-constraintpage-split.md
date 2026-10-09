# Goal-Loop 方向 96：constraintpage 1,830 行拆分——全仓新头号巨兽解体（域 TU + internal.h）

## 背景（实测事实，勿再勘察；行号为 2026-10-09 master `32851a1e` 口径）

方向 83（#312）拆掉主窗本体（2,137→915）后，**constraintpage.cpp
1,830 行升为全仓头号**：

- `src/ui/pages/constraintpage.cpp` **1,830 行**（+头文件）——
  约束页：单因素图全流程 UI（方向 73 两族约束线 + 井点因子
  提取 `wellFactorSection` + 插值方法 + 等值线主题；#295
  一笔 +417 行冲上来的）+ 方法参数段 + 结果表。
- 次级梯队：`seismic3dviewpanel.cpp` 1,800、`wellsectionscene.cpp`
  1,762、`datapreviewtabs.cpp` 1,758（+internal.h 1,607，
  **同族 3,365——TODOS.md:21-31 方向 80 拆分候选清单榜首**）、
  `datacatalog.cpp` 1,795、`catalogstore.cpp` 1,605。
- **拆分先例**：#312 主窗 7 域 TU（docks 600/ribbon 241/.../
  internal.h 60）；方向 65 canvas 2,192→297（12 TU + 2 内部
  契约头）；方向 66 三文件解体——模式成熟。
- **#308 触碰面**：constraintpage 未被 #308 触碰（其 diff 只
  含 geostat/singlefactor/workflow/测试）——但方向 91 若先
  合会加协变量面（`constraintpage.cpp:559-634` 区间），建议
  本方向后行或与 91 协调（谁先合谁为准）。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\constraintpage-split -b goal/constraintpage-split-20261010 origin/master
cd .worktrees\constraintpage-split
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。主回归面：
tst_factorworkflow/tst_mappingpages/tst_constraint_draw/tst_ui。

## 目标形态（建议按序）

1. **R0 结构勘察**：按 UI 域盘点（约束线绘制族/井点因子提取
  （wellFactorSection）/插值方法参数/等值线主题/结果表/方法
   说明）画段位图；与 typedconstraintdrawcontroller/
   constraintdrawcontroller（同页协作面）的边界核对；切分
   线记 ledger 定案。
2. **公共 API 冻结**：ConstraintPage 类定义公共区 diff 对拍
   零变更；消费方（ribbonpanels/paleomainwindow_attach_
   constraint）零改动。
3. **域 TU 拆分**：`constraintpage_<域>.cpp`（预计 5-7 TU：
   _wellfactors/_direction/_break/_interpolation/_style/_
   results）+ `constraintpage_internal.h`；每 TU ≤600 行
   目标；主文件 ≤800 行。
4. **行为红线**：objectName 全保留（#295 的 objectName 稳定
   命名是测试锚——`wellFactorSection` 等逐个对拍）；信号序
   /初始化序不变；方向 91 的协变量面（若已合）语义保留。
5. **测试口径**：tst_factorworkflow（150 行 #295 增量）/
   tst_mappingpages/tst_constraint_draw 零改动通过×2 遍。
6. **收口**：CMake 更新；include 纪律；候选清单更新
  （TODOS.md:21-31 划掉 constraintpage 项）。

## 通用纪律（方向内全程有效）

- **分层**：全部留 `src/ui/pages/`（视图层）；新文件头三行
  `// 层：视图`；`check_layering.py --strict` 绿。
- **行为保留红线**：objectName/信号序/初始化序逐条不变；
  「顺手改进」违规记 TODOS。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行；全量构建后再
  ctest。
- **无人值守**：切分线自行定案记 ledger；与方向 91 并行撞
  constraintpage 按「谁先合谁为准」。
- **ledger**：`.goal-loop-ledger-constraintpage-split.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （API 等价/objectName 守恒/信号序/TU 边界/无死代码 五维）
  → 修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 类零变更：ConstraintPage 公共区 diff 零变更；消费方零
   改动（rg 证据）。
2. 体量：主文件 ≤800 行；新 TU 各 ≤600 行（wc 证据入
   ledger）。
3. objectName 守恒：#295 引入的全部 objectName 逐个 rg 命中
  （清单入 ledger）。
4. 行为等价：tst_factorworkflow/tst_mappingpages/tst_
   constraint_draw 零改动通过×2 遍；全量 ctest 对照 R0 红
   集合 diff 为空。
5. layering 三档绿；构建无新警告；git diff --check 干净；
   TODOS 候选清单同步。
