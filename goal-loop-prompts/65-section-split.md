# Goal-Loop 方向 65：seismicsection 家族拆分——dockwidget 3,040 + canvas 2,192 新晋头号巨兽解体

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

八批拆分战果落账后（seismictaskservice 5041→242、attach
3816→702、datalist 3839→938、constraintfactorjobs 3693→249、
dataimportservice 2437→187），**全仓已无 3,000 行级文件，
seismicsection 家族是新晋头号**：

- `src/ui/seismicsection/seismicsectiondockwidget.cpp` **3,040
  行**（实测 Get-Content 口径）——剖面 dock 集成面板：属性/
  反演/断层控制器、卷帘对比、任意线、拾取、显示参数、任务
  接线全在一个 TU。审计时代 CONC-01 即在此文件。
- `src/ui/seismicsection/seismicsectioncanvas.cpp` **2,192 行**
  ——剖面画布：colormap 8 档、纹理缓存键=内容指纹×显示参数、
  拾取交互、overlay。两者合计 5,232 行。
- **拆分先例（仓内成熟范式）**：主窗族 `paleomainwindow.cpp
  1,908 + _workbench 712 + _sections 250 + _faults 27`（方向
  56）；datalist 3,839→938 + 7 个域 TU（方向 56）；
  seismictaskservice 五 TU（方向 55）——「按职责切 TU + 聚合
  头 + 内部 internal.h」三件套。
- **行为红线（seismic 面特有）**：迟到回调 QPointer 守卫、
  世代号+请求号双守卫、卷帘 B 图任务取消对端、纹理缓存键
  语义（方向 seismic-runtime-closure 建立的矩阵，tst_seismic_
  sectionui 23 例）——拆分逐条保留。
- **协调面**：本方向与方向 64（errorhub）都触 dockwidget
 （11 处 QMessageBox）——**本方向先行拆文件，64 后迁弹框**；
  若 64 已在飞按「谁先合谁为准」。canvas 的 colormap/纹理
  缓存是方向 43（attr-volume）消费面——公共 API 不变。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\section-split -b goal/section-split-20261007 origin/master
cd .worktrees\section-split
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。地震域测试矩阵（tst_seismic_sectionui 等）是
主回归面。

## 目标形态（建议按序）

1. **R0 结构勘察**：dockwidget 按控制器族盘点（属性/反演/
  断层/卷帘/任意线/拾取/显示参数/任务接线）画段位图与共享
   状态依赖；canvas 按职责盘点（渲染/缓存/拾取交互/overlay/
   colormap）——切分线记 ledger 定案。
2. **公共 API 冻结**：两头的类定义公共区 diff 对拍零变更；
   消费方（attach/seismic3d 链路）零改动。
3. **dockwidget 拆分**：按控制器族拆 TU
  （seismicsectiondock_<域>.cpp）+ internal.h；每 TU ≤900 行
   目标；每步地震测试绿再下一族。
4. **canvas 拆分**：渲染/缓存核与交互控制分 TU；纹理缓存键
   语义单独成节注释（消费面引用）。
5. **守卫语义对拍**：tst_seismic_sectionui 23 例零改动通过
  （跑三遍，先例口径）；UAF/迟到回调矩阵逐条断言。
6. **收口**：两主文件各 ≤1,000 行；CMake 更新；include 纪律
  （域间经 internal.h/公共头）。

## 通用纪律（方向内全程有效）

- **分层**：全部留在 `src/ui/seismicsection/`（视图层）；新
  文件头三行 `// 层：视图`；`check_layering.py --strict` 绿。
- **行为保留红线**：守卫语义/纹理缓存键/信号序/初始化序
  逐条不变；「顺手改进」违规记 TODOS。
- **测试口径**：地震域测试每批全绿；sectionui 矩阵收口跑
  三遍；全量构建后再 ctest（陈旧链接假红教训）。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：切分线自行定案记 ledger；与方向 64 的并行
  冲突按「谁先合谁为准」。
- **性能断言**：剖面渲染性能门不回退（比率门对照 R0，
  tst_seismic_perf/budgets）。
- **ledger**：`.goal-loop-ledger-section-split.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （API 等价/守卫语义/缓存键/TU 边界/无死代码 五维）→ 修复
  → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 类零变更：两头文件公共区 diff 零变更；消费方零改动
  （rg 证据）。
2. 体量：两主文件各 ≤1,000 行；新 TU 各 ≤900 行（wc 证据
   入 ledger）。
3. 守卫矩阵：tst_seismic_sectionui 23 例零改动通过×3 遍；
   迟到回调/世代号/卷帘取消语义断言原样绿。
4. 性能门：tst_seismic_perf/budgets 对照 R0 比率无 >10% 劣化。
5. 全量 ctest 对照 R0 红集合 diff 为空；layering 三档绿；
   构建无新警告；git diff --check 干净。
