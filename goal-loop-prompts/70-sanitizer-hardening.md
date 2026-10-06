# Goal-Loop 方向 70：Sanitizer 常态化与构建硬化——ASAN CI 化 + 预算豁免 + Unity 前置清障

## 背景（实测事实，勿再勘察；行号为 2026-10-06 master `f130fb2` 口径）

devex.md 递延 5/6 的代码化（`docs/progress/devex.md:142-160`
「Unity 实测」段 + `:182` 递延清单）：

1. **ASAN/UBSAN 只跑过一轮手动全量**：`build-asan`（534 目标
   全编过）ctest 80/83——ASAN 零内存错误、UBSAN 仅 3 处 Qt
   内部告警（qpointer.h:76 downcast 已知面）、3 个失败全是
   预算断言在 sanitizer ~2× 减速下的预期红（tst_correlation_
   full 墙钟/tst_perfbudget/tst_segy_lines RSS）。**无 CI 化、
   无周期复跑**（.github/workflows/ci.yml 四 job 零 sanitizer
   档；tools/ 无 asan 脚本）。
2. **`PALEO_SANITIZER_BUILD` 未落地**：rg 全仓只命中 devex.md
   文档，CMakeLists.txt 无此编译定义——三个 budget 测试降档
   的前置缺失（不减档则 sanitizer 下永红）。
3. **Unity build 编译不过（注释实证）**：`CMakeLists.txt:677-683`
   「ON 当前编译不过——全仓 .cpp 惯用同名匿名 helper
  （setError/connectionNameFor…），metadata/catalog/qgis 等
   目标合批即 redefinition」；`:684-689` 已接线 10 目标
   UNITY_BUILD TRUE + BATCH_SIZE 8（启用被注释挡住）。
   **冲突面量化**：文件级 `setError(QString*, const QString&)`
   定义 **35 个 .cpp**（metadata 9/qgis 7/workflow 7/io 5/
   catalog 2/algorithms 6 等）；`connectionNameFor` **8 个
  .cpp**（catalog 1 + metadata 7）——均为匿名命名空间内
   2-3 行同构实现，收拢是机械改动。
4. **构建加速缺当前规模基线**：ET14 承诺「增量 ≤60s」引用
   的 BUILDING.md:182 实测是 Phase 0「3 个目标」时代数据；
   当前 425 .cpp/534 目标无增量基线钉住（PLAN:1451 承诺
   与实测脱钩）。

## 环境接线（Windows 本机实测口径）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\sanitizer-hardening -b goal/sanitizer-hardening-20261007 origin/master
cd .worktrees\sanitizer-hardening
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 58。**注意**：本机是 MSVC——ASAN/UBSAN 开关
`CMakeLists.txt:68-82` 明确 MSVC 拒绝；sanitizer 验证档只能
在 Linux（CI 或 WSL）跑。本方向本机做 Unity 修复 + 定义接线
（可本机验证编译），sanitizer job 走 CI（用户已声明无需 CI
的门禁语义——CI yaml 属仓内代码，写进去但验收不等它跑）。

## 目标形态（建议按序）

1. **PALEO_SANITIZER_BUILD 落地**：CMakeLists 加编译定义
  （ASAN 或 UBSAN 开时任一开启即 define 1）；三个 budget
   测试（tst_correlation_full/tst_perfbudget/tst_segy_lines）
   读定义降档（预算×放宽系数，注释口径）；非 sanitizer
   构建零行为变化。
2. **Unity 前置清障**：`setError` 35 处 + `connectionNameFor`
   8 处收拢——共享 internal 头（`src/metadata/` 与
   `src/catalog` 各一或合一，按层规则定）+ 匿名空间去重
  （保留薄包装调用共享版）；逐模块合批验证。
3. **Unity 启用验证**：去重后重跑 build-unity 轮（paleo_store
   与 paleo_qgis 首轮合批即红的两个目标重点）——全目标合批
   编译过则把 CMakeLists「编译不过」注释改写为实测口径；
   仍不过则如实记录剩余冲突面。
4. **CI sanitizer job**：ci.yml 加 `linux-asan`（复用 linux
   job 模板 + PALEO_ENABLE_ASAN=ON + PALEO_ENABLE_UBSAN=ON，
   continue-on-error 起步——先观察再转阻断）；jobs 排版
   对齐既有四 job。
5. **增量基线钉住**：tools/ 加 `measure_incremental.sh`（touch
   单个 app 源文件 → 增量构建 → 计 TU/目标数与耗时入
   JSON——绝对时长仅记录，比率口径防跨机）；BUILDING.md
   ET14 段更新为当前规模口径。
6. **测试**：降档开关单测（定义 on/off 的预算差断言）；
   Unity 构建下全量 ctest（若全编过）。

## 通用纪律（方向内全程有效）

- **分层**：helper 收拢守层规则（metadata/catalog 各自内部
  头，不跨层 include）；`check_layering.py --strict` 绿。
- **行为红线**：helper 收拢零行为变化（同构实现合并，签名
   不变调用点机械替换）；非 sanitizer/非 unity 构建路径
   零变化（对拍）。
- **诚实面**：Unity 若仍有冲突如实记录不硬启；CI job 不等
   跑（用户门禁语义）；增量时长是记录不是门。
- **资源**：`-j8`；ctest 串行；unity 构建轮次多——增量验证。
- **无人值守**：internal 头放位自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-sanitizer-hardening.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （分层/零行为变化/定义接线正确/Unity 验证真实性/文档同步
  五维）→ 修复 → 再 review，至少两轮零 High/Medium；Low 记
  PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. PALEO_SANITIZER_BUILD：定义接线证据（CMake 片段）；三个
   budget 测试降档逻辑单测；非 sanitizer 构建对拍零差异。
2. Unity 清障：setError/connectionNameFor 文件级定义计数
   35+8 → 收敛到共享头 1+1 处（rg 证据）；paleo_store 与
   paleo_qgis 合批编译过（构建日志证据）。
3. Unity 终态：10 目标 UNITY_BUILD 全编过 + ctest 绿（或
   如实记录剩余冲突清单）；CMakeLists 注释与实测一致。
4. CI job：ci.yml 含 linux-asan（yaml 语法验证本地可做：
   动作解析或 actionlint 类工具若不可用则人工核对缩进/
   键名与既有 job 一致）；不等待其运行。
5. 增量基线：measure_incremental.sh 可跑（本机或说明
   Linux-only）；BUILDING.md ET14 更新为当前规模口径。
6. 全量 ctest 对照 R0 红集合 diff 为空；layering 三档绿。
