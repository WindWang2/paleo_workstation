# Goal-Loop 方向 50：IO 稳健性攻坚——SEG-Y/井文件解析容错 + 深度基准诚实面

## 背景（实测事实，勿再勘察；行号为 2026-10-05 master `adf2be7` 口径）

数据读面是全站的地基，审计确认的未修项集中在此：

- **BIZ-05**：`src/io/segyreader.cpp:579-588` SEG-Y 解码无 NaN/Inf
  清洗——坏样点直接进管线污染下游属性/反演/统计。
- **BIZ-06**：`src/io/wellfileparsers.cpp:94-117` 无深度井顶仍 append；
  哨兵仅 -99999（`:17` `kNullSentinel = -99999.0`，`:24-29` 数值列
  -99999 视为空）——其他惯用空值（-999.25、空白列）静默当真值。
  `src/io/timedeptool.cpp:25-28` 同口径。
- **BIZ-14**：`src/workflow/sectionworkbench.cpp:281-287` 深度单位
  同义词被拒（METER/METERS 等大小写/复数变体）——同类同义拒收
  在数据导入面是高频摩擦点。
- **BIZ-07/10/12（待证，R0 复核）**：SEG-Y 非标准字段/异常 trace
  头字段序（BIZ-07）、截断文件行为（BIZ-10）、xlsx/坐标表边角
  （BIZ-12）——待证 15 项中的 io 域三席。
- **既有纪律先例**：`segyreader.cpp:916` 扫描偏移已计入
  `m_badTraceOffsets`（BIZ-03 先例）、`:1140-1162` anyDifferent
  探针（BIZ-04 先例）——本方向沿用「计数+上报，不静默」口径。

## 环境接线（Windows 本机实测口径，源 goal/sf-kriging 账本 R0）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\io-robustness -b goal/io-robustness-20261006 origin/master
cd .worktrees\io-robustness
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 48（PATH 顺序/沙箱临时目录——io 测试重依赖 QTemporaryDir，
R0 基线对照必须先行）。

## 预算（开发量）

- **Agent tokens**：上限 3 亿，预计 0.7–1.0 亿（修复为主 + 夹具
  生成 + 回归测试）。
- **执行花费**：10–14 轮构建+测试，预计墙钟 1.5–2 个工作日会话。

## 目标形态（建议按序）

1. **R0 复核**：BIZ-07/10/12 逐条带证据定性（同方向 48 口径），
   already-fixed 记 commit 号。
2. **NaN/Inf 清洗**：segyreader 解码路径 NaN/Inf → 置 null 并计数
   进 badTraceOffsets 同族通道；下游（属性/反演/统计）消费 null
   口径核对（既有 null 传播语义不得回归）。
3. **井顶/深度哨兵收口**：哨兵词表化（-99999、-999.25、LAS 常用
   null、空白/非数值列）统一进 domain 常量；无深度井顶不 append
   改进「拒收+列因」进 importledger（先例：welllog-fmt 方向 44 的
   失败诚实面）；timedeptool 同步口径。
4. **深度单位同义词**：sectionworkbench 单位解析改规范词表
  （大小写/复数/常见别名 M/FT/METER/METERS），未知单位如实报错
   不猜；测试钉死同义表。
5. **夹具矩阵**：坏样点 SEG-Y、-999.25 井表、截断 SEG-Y、单位
   别名井文件的夹具各一（生成脚本进 tests 或 tools/reference，
   先例：singlefactor 的 make_outsource_fixtures.py）。

## 通用纪律（方向内全程有效）

- **分层**：解析容错归 io，哨兵/单位词表归 domain，消费面核对
  归 workflow；`check_layering.py --strict` 绿。
- **诚实面**：清洗/拒收/降级逐条计数上报，零静默；哨兵命中数
  进导入清单可见面。
- **回归红线**：既有 tst_seismic_engine / tst_wellfileparsers /
  tst_welllogset 全绿；清洗不改变好数据路径的数值（对拍断言）。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：决策自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-io-robustness.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/
  数值不变性/诚实面/夹具真实性/i18n 五维）→ 修复 → 再 review，
  至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. NaN/Inf 夹具：坏样点计数进读面报告，样点置 null 后下游
   属性计算结果与「手工剔除坏样点」直算一致（对拍断言）。
2. 哨兵词表：-999.25/-99999/空白列夹具逐个「拒收+列因」，
   零静默真值；好数据对拍不变。
3. 单位同义：METER/METERS/m/ft 别名夹具全部正确解析或诚实
   报错；词表测试钉死。
4. BIZ-07/10/12 复核终态入 AUDIT_ISSUES.md（同方向 48 口径）。
5. 回归：地震/井文件既有测试全绿两遍；R0 基线红清单外的零
   新增失败。
6. check_layering 三档绿；check_i18n 绿。
