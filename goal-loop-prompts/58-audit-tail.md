# Goal-Loop 方向 58：审计尾巴清零——BIZ-07/RUNTIME-03/BIZ-10/BIZ-11 四项无主债务收口

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

八批方向 48（audit-closure）与 50（io-robustness）收口后，
AUDIT_ISSUES.md 名义还剩 5 处「仍存在」标记，其中 TEST-06 的
高中危面已被方向 52 清零（标记未同步，低危残余约 17 头）。
**有效 open = 4 项，全部无主**
（48 移交 50 的七项中 50 只收了 BIZ-05/06/14）：

1. **BIZ-07**（P2）：配准参数无非有限校验——`src/workflow/
   registration.cpp:71-76` `p.tx/ty/sx/sy/rotDeg` 直接 `toDouble()`
   无 `std::isfinite`，NaN/Inf 配准参数直进 GDAL 变换。
2. **RUNTIME-03**（P2）：LasCache 合乘者 issues 丢失——
   `src/io/lascache.cpp:241-269` InflightCoalescer 的 lambda 引用
   捕获首个提交者的 `issues` 指针，同指纹并发 rider 的 issues
   永不回填（`src/io/lasdoc.h` 的 LasDoc 无 issues 字段，
   lasdoc.h:13-22）。
3. **BIZ-10**（P2，标记与代码冲突）：工程 CRS 致命中止——审计
   要求的 `transform 前 isValid 前置`在全部三处调用点已存在
  （`src/algorithms/paleoalgorithms.cpp:358`（git 溯源引入于
   ddcd974，2026-09-25 早于审计）、`distancetransform.cpp:108`、
   `mincurvature.cpp:151`）。需按 AUDIT_ISSUES.md:1513-1537 原始
   复现路径实跑定性：残余在别的场景还是标记漂移，然后改对标记。
4. **BIZ-11**（P2，窄残余）：未投影 GeoTIFF——共享写口已带 CRS
  （`welldist.cpp:98-100`、`distancetransform.cpp:168-170` 走
   `PaleoRasterOut::createFloatRaster(..., sourceCrs()...)`），
   残余仅剩非 LOCAL_GRID_WKT 直调场景——扫全仓 `createFloatRaster`
   调用点逐个定性。
4. **TEST-06 标记同步与低危清单**：方向 52 已清零 16 个高中危
   头（账本 R3 §5）；把 AUDIT_ISSUES.md:1828 标记改对（TEST-06
   原记 33 头零测试）+ 剩余低危零测试头（约 17 头）列清单——
   逐头给「已有测试/待补/不值得」终态建议，不强求全补。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\audit-tail -b goal/audit-tail-20261007 origin/master
cd .worktrees\audit-tail
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
ctest --test-dir build   # Windows 串行；Qt 混链基线红先对照（见方向 72）
```

坑位（实测沿用）：PATH 里 `paleo-qgis-deps/Library/bin`（Qt 6.11）
须在 `C:/deps/Qt/6.8.0/msvc2022_64/bin` 前；本机全量回归有
环境红（185/293，Qt 混链，方向 72 根治）——R0 先跑基线 ctest
记红集合，验收按「红集合 diff 为空」口径。

## 目标形态（建议按序）

1. **R0 复核**：四项 + TEST-06 标记逐条带证据定性（BIZ-10 按
   原始复现路径实跑：构造工程 CRS 输入跑 ConstraintIDW/
   DistanceTransform，观察是否中止）。
2. **BIZ-07 修复**：registration 参数解析加 isfinite + 范围闸
  （sx/sy 零缩放同拒），拒收路径列因进错误通道；夹具：NaN/Inf/
   零缩放参数的配准输入。
3. **RUNTIME-03 修复**：LasCache rider 的 issues 回填——LasDoc
   加 issues 字段或 rider 各自 issues 出参（勘察 InflightCoalescer
   合乘契约后择一，注释钉死）；并发测试：两线程同指纹请求，
   双方 issues 均非空。
4. **BIZ-11 收口**：全仓 `createFloatRaster` 调用点清单化，非
   LOCAL_GRID_WKT 场景逐个补 CRS 或改走共享写口。
5. **AUDIT_ISSUES.md 终态**：四项 + TEST-06 全部带终态标记
  （修了给 commit、不复现给复现方法、窄残余给清单）。

## 通用纪律（方向内全程有效）

- **分层**：修复归各层（registration=功能、lascache/lasdoc=数据）；
  `tools/check_layering.py --strict` 绿。
- **诚实面**：修复前先复现（红）后修（绿）；BIZ-10 若定性为
  标记漂移，如实记「审计时点与当前代码的差异」不冒记已修。
- **资源**：构建/测试一律 `-j8` 以内；ctest 串行。
- **无人值守**：四项定性自行定案记 ledger，不等人工确认。
- **ledger**：`.goal-loop-ledger-audit-tail.md`，每轮记
  「改动/验证/判定/下一步」。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/断言真实性/行为保留/文档同步/i18n 五维）→ 修复 →
  再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. BIZ-07：NaN/Inf/零缩放配准夹具拒收 + 列因（修前红/修后绿
   证据）；正常参数路径回归不变（对拍）。
2. RUNTIME-03：并发合乘测试双方 issues 均回填（计数断言）；
   单请求路径回归不变。
3. BIZ-10：实跑定性记录（复现路径 + 结果）入 ledger；AUDIT
   标记与实跑结论一致。
4. BIZ-11：`createFloatRaster` 调用点清单终态（全带 CRS 或
   列因）；新夹具（无 CRS 输入）产出 GeoTIFF 带 CRS 或拒收。
5. AUDIT_ISSUES.md：44 个原始条目 + 后续新增注记项全部终态
   （口径：AUDIT_ISSUES.md 全部 ID 零「仍存在且无注记」）；
   TEST-06 标记同步且低危残余清单化。
6. 全量 ctest 对照 R0 红集合 diff 为空；layering 三档绿。
