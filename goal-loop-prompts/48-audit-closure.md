# Goal-Loop 方向 48：审计清账——AUDIT_ISSUES 未修项修复 + 假绿测试清零

## 背景（实测事实，勿再勘察；行号为 2026-10-05 master `adf2be7` 口径）

AUDIT_ISSUES.md（2026-10-01 审计）44 个活跃问题，2026-10-05 三路
深度复核结论：**21 已修（含全部 P0）/ 8 未修 / 15 待证**。本方向把
这本账清零。注意教训：审计快照会漂移（旧式 SLOT() 复核时 24 处已
变 6 处）——R0 必须逐条重验，already-fixed 记档不重修。

**本方向负责的未修项（8 项中的 5 项 + 待证中的非 BIZ/非 TEST-06 系）**：

1. **TEST-07**（高）：`tests/tst_panels.cpp:4875` offscreen 焦点遍历用
   `QTest::keyClick(w, Qt::Key_Tab)` 模拟（CI 红）——改
   `nextInFocusChain()` 链断言，勿依赖平台焦点事件。
2. **RUNTIME-04**：`src/services/seismictaskservice.cpp:632-638` timeout
   处理器内同步 `delete timer`（`:635`）——处理器栈上对象自杀，改
   `deleteLater`。
3. **BIZ-09**：`src/ui/edittools/vertexeditortools.cpp:1125` 拓扑容差
   `1e-9` 硬编码（`:1122` 注释自述「R-tree 邻域候选（1e-9 包络）→
   qgsDoubleNear 精确过滤」）——提取具名常量并注释量纲依据。
4. **ARCH-05**：`src/algorithms/paleoalgorithms.cpp:124` 仍调
   `DataCatalog::localGridCrsWkt()`——algorithms（数据层）反向耦合
   catalog（数据层兄弟模块）取全局态。解法：CRS WKT 由调用方传入
   参数化，算法核不再问 catalog。
5. **TEST-04**（中）：四处恒真自比较断言（假绿）：
   `tests/tst_decorations.cpp:59`、`tests/tst_wellcomposite_shell.cpp:387`、
   `tests/tst_layoutexport.cpp:56`、`tests/tst_layoutshell.cpp:88`——
   逐处换成真实不变量断言（先例：TEST-03/05 在 #151 的换法）。

**明确不在本方向（避免撞面）**：BIZ-05/06/14 与 BIZ-07/10/12 待证归
方向 50（io-robustness）；TEST-01/02/06 待证与零测试头交叉归方向 52
（test-deepening）；TEST-07 的 tst_panels 其余用例不动。

**待证 15 项复核清单（R0 逐条定性）**：MEM-06/07（`src/ui/edittools/
editingtoolbar.h:150` `mCanvas` 仍裸指针，其余成员已 QPointer 化）、
CONC-04/05、RUNTIME-02/03（若属 AI 远端域移交方向 51 口径）、
ARCH-03/06/07。每项终态三选一：仍存在→修复；已修→记 commit 号；
不复现→记复现方法。

## 环境接线（Windows 本机实测口径，源 goal/sf-kriging 账本 R0）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\audit-closure -b goal/audit-closure-20261006 origin/master
cd .worktrees\audit-closure
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
ctest --test-dir build   # Windows NativeFormat 侧串行
```

坑位（实测）：运行时 PATH 里 `paleo-qgis-deps/Library/bin`（Qt 6.11）
必须排在 `C:/deps/Qt/6.8.0/msvc2022_64/bin` 之前，否则
`ENTRYPOINT_NOT_FOUND`（0xC0000139）；本沙箱曾拒绝对测试子进程的
临时目录写入（既有 12 项基线红）——R0 先跑基线 ctest 对照，区分
环境红与本方向红。Linux 先例的 vendor symlink 不适用（依赖绝对前缀）。

## 预算（开发量）

- **Agent tokens**：上限 3 亿，预计 0.6–0.9 亿（复核+修复+测试编写，
  无大规模生成）。
- **执行花费**：8–12 轮构建+测试（Windows 增量每轮约 10–20 分钟），
  预计墙钟 1–2 个工作日会话。

## 目标形态（建议按序）

1. **R0 复核**：8 未修 + 15 待证逐条重验（带 rg/读码证据），
   already-fixed 记 commit 号；定案本方向实际修复清单。
2. **修复五项**：每项「修复 + 回归测试钉死 + AUDIT_ISSUES.md 状态
   更新」三件套；TEST-07 修前先本地复现红（offscreen 平台）。
3. **待证复核**：15 项定性记档；仍存在且属本方向的小修直接修
   （MEM-06 editingtoolbar.h:150 QPointer 化预计在内）。
4. **TEST-04 恒真断言**：四处换真实不变量（每处写明替换理由——
   原断言为何恒真、新断言验证什么）。
5. **ARCH-05 解耦**：algorithms 侧零 catalog include（`rg` 证据），
   CRS WKT 参数化后消费面（workflow/ui 调用点）全量对齐。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；ARCH-05 修复后
  `tools/check_layering.py --strict` 必须绿。
- **无人值守**：所有决策点（复核定性、修复取舍）自行勘察定案并
  记 ledger，不等人工确认；歧义按「诚实面优先」裁决。
- **资源**：构建/测试一律 `-j8` 以内；ctest 串行。
- **测试证据**：每个修复先有红后见绿（修前失败复现或断言性论证），
  不接受「应该没问题」。
- **性能断言**：禁绝对毫秒墙钟。
- **ledger**：`.goal-loop-ledger-audit-closure.md`，每轮记
  「改动/验证/判定/下一步」。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/
  断言真实性/行为保留/文档同步/i18n 五维）→ 修复 → 再 review，
  **至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. AUDIT_ISSUES.md 44 项全部有终态：已修项带 commit/PR 号、待证项
   带复核证据、不复现项带复现尝试记录；零「open 且无注记」残留。
2. 五个负责项各有回归测试：修前红/修后绿证据入 ledger（TEST-07
   至少有 offscreen 下用例通过的证据）。
3. TEST-04 四处恒真断言清零：新断言能被故意注入的缺陷打红
   （mutation 式验证至少一处示范）。
4. ARCH-05：`rg "catalog/" src/algorithms/` 零命中；消费面 CRS
   传参对齐后地震/成图域测试全绿。
5. `check_layering.py` 三档绿；全量 ctest 两遍无新增失败（对照
   R0 基线红清单，环境红除外但需记档）。
6. 文档：AUDIT_ISSUES.md 更新与本批 diff 一致；TODOS.md 若有
   对应递延条目同步划掉。
