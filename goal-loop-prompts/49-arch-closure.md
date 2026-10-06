# Goal-Loop 方向 49：架构收口——文档漂移修复 + 检查器强化（传递闭包/词表/死代码）

## 背景（实测事实，勿再勘察；行号为 2026-10-05 master `adf2be7` 口径）

架构合规本体全绿（六层护栏/反向 include 零/224 个 UI 类全量
反查无越层 new），但存在**契约面漂移与检查器盲区**：

1. **文档漂移 ×3**（文档是 agent 协作契约面，漂移会被后续
   prompt 当事实引用）：
   - `docs/PALEO_QGIS_PLAN.md:1488` 与 `tools/check_layering.py:14`
     docstring 写「四头」（metadata 白名单），实际
     `tools/layering_vocab.json` 为**六头**（layermanifest/
     paleoprojectstore/mapversionstore/releasestore/wellsectionstore/
     faultsetstore）。
   - `goal-loop-prompts/README.md` 标题「35 个方向」——实为 47
    （注：第八批 PR 已把 README 标题/计数更新为 57 并补表，
    本项 R0 确认即可，勿重复改）。
   - `AGENTS.md:22`（六头）与 PLAN（四头）互相矛盾——AGENTS
     是对的。
2. **R4 传递性 QtWidgets 缺口**：linkage 经 `src/qgis/seismicsectiontool.h`
   （2 处消费）间接引入 QtWidgets——「功能层无 QtWidgets」仅在
   直接 include 级成立；checker 无传递闭包检查。
3. **checker 盲区**：`tools/check_layering.py` 只扫 include 不查
   符号级依赖（「不带 include 直接 new」理论盲区，当前 0 实例）；
   `tools/ui-token-exceptions.json` 达 **119KB**——例外清单体量
   本身即债，无收缩门禁。
4. **疑似死代码**：`src/qgis/semanticlegend.h` 零外部消费
  （`rg SemanticLegend src/ tests/` 复核 + git log --follow 定
   性：死代码则删，方向 31 预留则记档）。
5. **paleo_core 伞命名漂移**：PLAN/UI_LAYER_PLAN 中 paleo_core
   描述与 CMake 现状（INTERFACE 兼容伞，不再编代码）口径
   对齐核查。

## 环境接线（Windows 本机实测口径，源 goal/sf-kriging 账本 R0）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\arch-closure -b goal/arch-closure-20261006 origin/master
cd .worktrees\arch-closure
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

坑位同方向 48。checker 开发用 python 直接跑（tools/ 下脚本，
ctest 里 layering 三项为门禁）。

## 预算（开发量）

- **Agent tokens**：上限 3 亿，预计 0.6–0.9 亿（文档+脚本+少量
  代码定性）。
- **执行花费**：8–12 轮（构建轮次少——代码面小），预计墙钟
  1–1.5 个工作日会话。

## 目标形态（建议按序）

1. **文档收口**：三处漂移修复（PLAN/checker docstring 改六头
   实况、README 计数项已随第八批修复——R0 核实即可、AGENTS 与
   PLAN 对齐）；paleo_core 口径对齐；全仓 rg「四头/六头」残留清零。
2. **semanticlegend 定性**：消费面复核（src+tests 全量 rg）+
   git log --follow 溯源——死代码删除（build 验证零引用）；
   方向 31 预留则头注释记用途与回收条件。
3. **checker 传递闭包档**：check_layering.py 新增 opt-in
  `--transitive` 档（默认关，不进 strict 闸）——从 ui 层头起
   BFS 解析 include 图，报「视图/功能层经 qgis 封装头传递引入
   QtWidgets」黄牌清单（先解决 R4 实例：linkage 消费
   seismicsectiontool 改意图信号——先例 threewaylocator 信号化；
   若改造面失控则该文件注记递延，仅落 checker 能力）。
4. **符号级抽检档**：`--symbol-audit` opt-in——非视图层 TU 内
   `new <Ui类名>` 正则抽检（词表来自 ui 头类定义扫描），当前
   0 实例应跑出零报告（护栏+1，防回流）。
5. **token 例外收缩门**：ui-token-exceptions.json 例外计数入
   baseline ratchet（同 layering-baseline 先例）——非严格模式
   提示、strict 模式只降不升；R0 记当前计数，本方向**不**大规模
   清理例外（清理属 DESIGN 域递延）。
6. **文档同步**：PLAN/UI_LAYER_PLAN 补新档位说明；checker
   selftest 夹具补传递闭包与符号抽检的正反例。

## 通用纪律（方向内全程有效）

- **分层**：checker/docs 改动无层标记要求；若触 src/qgis/
  linkage 代码（R4 信号化），守既有层规则；`check_layering.py
  --strict` 绿。
- **不破坏既有门禁**：新增档 opt-in，默认行为与三档输出零
   变化（既有 CI/ctest 口径不红）；selftest 夹具新增不影响
   旧夹具。
- **资源**：`-j8`；ctest 串行。
- **无人值守**：漂移修复以「实测定据」为准（rg 现势），不猜
  历史意图；定性结论记 ledger。
- **ledger**：`.goal-loop-ledger-arch-closure.md`。
- **多轮 review（硬要求）**：每批 → checker 三档+新档自测绿
  → diff 自审（文档实据/门禁不回退/脚本质量/夹具正确/无越权
  清理 五维）→ 修复 → 再 review，至少两轮零 High/Medium；
  Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 文档：三处漂移修复后 rg「四头」零命中（PLAN/checker
   docstring 口径）；README 计数与目录文件数一致（57+README）；
   AGENTS/PLAN 无矛盾条目。
2. semanticlegend：终态（删除→构建全绿零引用证据；或预留→
   注记+回收条件）入 ledger。
3. 新档位：`--transitive` 在当前树上输出 R4 黄牌（linkage×2
   处可复现）；`--symbol-audit` 输出零报告；两档 selftest
   正反例夹具命中（mutation 式验证：故意造违规能被抓住）。
4. 既有门禁：layering 三档输出与 R0 基线逐字节一致（新档
   opt-in 不扰默认）；ctest layering 项全绿。
5. token 例外 ratchet：R0 计数入 baseline；故意加一条例外
   strict 打红（mutation 验证后撤销）。
6. 全量 ctest 无新增失败（对照 R0 基线红清单）。
