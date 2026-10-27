# Ledger — 方向 66：wellcomposite 家族拆分

- 基线：origin/master `33f496a`（任务书口径 f130fb2，按指示跟随 origin/master）
- worktree：`.worktrees/composite-split`，分支 `goal/composite-split-20261007`
- 构建：Ninja + MSVC（vcvars64）+ Qt Tools CMake 3.29.3 + vcpkg ninja；-j8

## R0 段位图与切分线（定案）

### 消费面核对

- 头文件三份（track 419 / panel 249 / canvas 333 行）**零改动**（公共 API 冻结）。
- `src/domain/wellcompositemodel.h` 零改动（domain 契约不动）。
- 无文件级辅助交叉：track.cpp 无匿名 ns/static；canvas.cpp 仅 `ppmForScaleRatio`
  （static，仅 resetZoom/setScaleRatio 用 → 留主文件）；panel.cpp 仅匿名 ns
  `s_faciesFactory`（setFaciesWorkflowFactory 定义 + 构造函数用 → 留主文件）。
- Q_OBJECT 全在头（不动），moc 不受 TU 拆分影响。
- CMake：paleo_ui 源列表 CMakeLists.txt L634-636，新 TU 直接追加。

### wellcompositetrack.cpp（1,875 行）→ 主文件 + 4 TU

按类族切（任务书三域中「道头渲染」=WellTrack 基类节留主文件单独成节注释；
「曲线充填」=trackcurves；「道配置」随各类 TU）：

| TU | 段 | 行区间（1-based，含） | 估算行数 |
|----|----|------|------|
| wellcompositetrack.cpp（主） | include + namespace + WellTrack 基类（D1.11 paintHeaderChrome/elideTitle 截断语义） | 1-76 | ~85 |
| trackpatterns.cpp | LithologyPatternFactory + FaciesPatternFactory | 78-460 | ~400 |
| trackcolumns.cpp | 六基础道（DepthScale/Text/Formation/Lithology/Core/Image）+ 其访问器（1776-1845） | 462-876, 1776-1845 | ~540 |
| trackcurves.cpp | CurveTrack + SymbolTrack | 878-1241 | ~380 |
| trackcompound.cpp | StratigraphyCompoundTrack + FaciesCompoundTrack + FaciesCompound 访问器（1847-1873） | 1243-1767, 1847-1873 | ~570 |

### wellcompositepanel.cpp（1,640 行）→ 主文件 + 5 TU

| TU | 段 | 行区间 | 估算 |
|----|----|------|------|
| wellcompositepanel.cpp（主） | include + 匿名 ns s_faciesFactory + setFaciesWorkflowFactory + 构造/析构 + openCurveConfigDialog/setWellName/setProjectName/setSourceDataPath | 1-82, 474-537 | ~165 |
| panelsetup.cpp | setupUi | 84-472 | ~410 |
| paneldata.cpp | 数据装配（XML 同步/异步 + LAS + setupTracksFromData） | 538-909 | ~400 |
| paneltracks.cpp | syncSessionToTracks + facies 绑定/预测 + D1.x 道操作槽 + specs 增量 + 会话记忆 | 911-1170 | ~280 |
| paneldepth.cpp | D2.x 深度交互槽（区间/pin/goto/书签/单位/gap）+ D1 深度装配链（深度表/TWT/读数） | 1171-1423 | ~275 |
| paneledit.cpp | D3.x 编辑模式 + saveDerived + marker + sidecar + strat + D4.x 导出/打印/预设 | 1424-1638 | ~235 |

### wellcompositecanvas.cpp（1,549 行）→ 主文件 + 2 TU

| TU | 段 | 行区间 | 估算 |
|----|----|------|------|
| wellcompositecanvas.cpp（主） | WellCompositeCanvas 全部（含 static ppmForScaleRatio） | 1-701 | ~710 |
| compositeheader.cpp | WellCompositeHeader（拖拽换位/分隔线/焦点环） | 702-961 | ~270 |
| compositebody.cpp | WellCompositeBody（绘制/手势/右键菜单/橡皮筋/标志层拖拽） | 963-1548 | ~595 |

### 回归面

- tst_wellcomposite{,_a11y,_depth,_editing,_framework,_multiwell,_perf,_shell,_visual}
  （9 项；perf 标 RUN_SERIAL 输出 PERF-BENCH 行；visual 标 golden）
- 性能门基线：R0 跑 tst_wellcomposite_perf 抓 PERF-BENCH 行，验收 ≤10% 劣化。

## 迭代记录

| 轮 | 改动 | 验证结果 | 通过/未通过 | 下一步 |
|----|------|---------|------------|--------|
| 0 | 环境接线（worktree + configure 17.8s 绿）；R0 勘察 + 切分线定案（本文） | 全量构建 1756/1756 绿 | 通过 | R0 ctest 基线 |
| 1 | R0 基线：全量 ctest（40% 过，194 红为本机环境项：GUI/IO 密集测试 Unix 路径假设 + qgis DLL 链 Qt6.11 入口点）；perf 基线 render 6ms/decimate 1244pts/sample 0ms/mem 0.0MB | 红集合快照 `red_r0_names.txt`（194 项） | 通过 | 执行切分 |
| 2 | 执行切分（行区间切片，段字节级不变）+ CMake 登记 + CRLF 统一 + layering 三档绿（静态） | dry-run 与真切分均验证：TU 段与原区间字节一致 | 通过 | 增量构建 |
| 3 | 增量构建修复 include×5（panelsetup+workflow/paleoviewport、paneldata+legendwidget、paneltracks+hiddentrackbar、paneldepth+QClipboard、主文件 editsession）；主文件 include 收紧 | 构建 102/102 绿；警告面与 R0 等价（C4458×4/C4100×1 均为原 panel.cpp 既有搬家，零新增） | 通过 | 测试对照 |
| 4 | 测试对照 ×2 遍：wellcomposite 族 6 红名单与 R0 完全一致（shell/framework/depth/editing/visual/multiwell），失败用例逐条同点（sidecar/IO 类环境红）；3 绿（主/a11y/perf）不变；perf render 4ms(R0 6ms)/decimate 1244 持平/sample 0-1ms/mem 0.0 —— 无劣化 | layering 三档绿 ×2 | 通过 | 全量 ctest diff + review |
| 5 | 行级多重集验证：原 3,968 非空代码行 → 拆分后 0 丢失、+3 行（访问器节注释在 columns/compound 各一份） | git diff --check 干净；体量：主文件 70/110/695（≤800），新 TU 最大 602（≤900） | 通过 | 全量 ctest 终态对照 |

### R0 既有红项归因（拆分不背锅证据）

主仓 build（未拆分代码）直跑同款失败：tst_wellcomposite_shell 0xc0000139（paleo_io 链引入的 QGIS provider DLL
入口点冲突）；framework/depth/editing/visual/multiwell 的 sidecar/store.save/img.save/f.open
失败为测试写临时文件的 Unix 路径假设（Windows 本机环境差异）。

### include 重组说明（非行为变更）

- 原 panel.cpp 的死 include（intervaleditor.h 无符号使用）与重复 include
  （curveconfigdialog.h/wellpositionlegendwidget.h 各两次）不带入新 TU。
- 各 TU include 按需精选（datalist 先例风格），编译器验证闭环。
- 主文件三份 include 收紧至本体符号集（track: paleotheme+主头；canvas:
  +QKeyEvent/QResizeEvent/cmath/trackops；panel: +curveconfigdialog/derivedsink/
  editsession/paleotheme/QFile/QFileInfo/QShortcut/utility）。
