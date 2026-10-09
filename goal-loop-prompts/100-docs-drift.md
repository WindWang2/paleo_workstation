# Goal-Loop 方向 100：文档漂移与账本入库纪律——PLAN/层计划刷新 + 三份脱队账本收编 + 漂移护栏

## 背景（实测事实，勿再勘察；行号为 2026-10-09 master `32851a1e` 口径）

四轮盘点发现**文档与账本的系统性漂移**——文档是 agent 协作
契约面，这批漂移会误导后续会话：

1. **架构文档滞后于 #312**：`docs/UI_LAYER_PLAN.md:426/:457`
   仍写「paleomainwindow.cpp 落点 ~2128 行（≤~2200）」——
   #312 已拆到 915 行 + 21 个家族 TU（实测）；W7 完成条件
   表全是旧口径。`docs/PALEO_QGIS_PLAN.md` 同期未刷。
2. **三份账本脱队**：
   - `.goal-loop-ledger-sf-geostat.md` 只存在于 #310 分支
     （`origin/goal/sf-geostat-20261009`），**它是 #310 的
     账本，不是 #308 的账本**。master 上 #308 是 squash
     `82ea9cd9`，该提交没有账本文件。缺的是一份用 #308
     自己的提交/测试重建的交付说明，不是把 #310 的
     1H/5M/7L 改贴成 #308 证据。
   - 该 #310 账本断言「MAPPING_WORKBENCH.md 不存在于 master」
     **失实**（f31ee461 已存在，且被 82ea9cd9 修改）——这是
     #310 勘察错误，不是 #308 的结论；
   - `env-debt2` 账本结尾仍写「PR 保持草稿…不标 Oracle
     已通过」而 #304 已合入——读者会误判方向状态。
3. **AUDIT_ISSUES.md 头部计数漂移**：`:15-25` snapshot note
   仍写「243 CTest suites」——当前口径 363-377 项（env-debt2
   R9/mkproject R1 观测）。
4. **SGS 语义未同步**：`TODOS.md:109`「Kriging/SGS 的逐线
   屏障语义：现方法不消费约束线」——#308 后「克里金完全不
   消费约束」已过时（本地方向克里金已消费方向线/软边界），
   但 geostat 核的 kriging/cokriging/sgs 隔断拟合并未因此
   完成。方向 91 按两条链修；本方向只做兜底注记，谁先合谁
   为准。91 先行则本项变确认，且不得把 `resolveVariogram`
   窄增量写成三条 geostat 方法已消费。
5. **lupdate 骨架**：#311 账本自记「.ts 翻译骨架未随分支
   更新（lupdate 26K 行 churn，留标准脚本收口）」——#246
   后新增大量 tr() 文案的 .ts 同步债。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\docs-drift -b goal/docs-drift-20261010 origin/master
cd .worktrees\docs-drift
./paleo-dev.ps1 build    # lupdate 需要 qt6 工具链（conda 布局 PATH 已由 ps1 内置）
```

等价手工接线见 BUILDING.md:140-152。

## 目标形态（建议按序）

1. **架构文档刷新**：UI_LAYER_PLAN.md 补「W8：主窗二次拆分
   （#312）」段——现状数字（915 + 21 TU 家族表）、新 internal.h
   契约、装配根归并审视结论；PALEO_QGIS_PLAN 层描述同步
  （catalog/ai/chat 新面一句话级更新，不重写）。
2. **账本收编**：
   - **#310 账本档案**（不是 #308 账本）：从
     `origin/goal/sf-geostat-20261009` 摘取
     `.goal-loop-ledger-sf-geostat.md`，入库标题/注记必须
     写成「#310 账本 / 平行实现」。原文保留，包括第 5 轮
     1H/5M/7L 与第 6 轮 0H/1M/5L。**禁止**把这些 review
     轮次写成 #308 的证据，也禁止把文件叫做「#308 账本」。
     「MAPPING_WORKBENCH.md 不存在于 master」加注记：这是
     #310 勘察时的失实句（文件在 f31ee461 已在，82ea9cd9
     还改过它）。注记即可，不要删原文，不要改记到 #308。
   - **#308 交付说明另写**，材料只来自 #308 自己的提交与
     测试：squash `82ea9cd9` 的 message 与 diff（含
     `docs/workflows/MAPPING_WORKBENCH.md`、
     `src/algorithms/singlefactor/localidw.cpp` /
     `krigingsurface.cpp`、`tst_singlefactor_kriging` /
     `tst_geostat_cokriging` / `tst_factorworkflow`，提交
     说明写 40/40）。不要从 #310 账本倒推 #308 做了什么。
     增量面仍由方向 91 处置，不要用 #310 账本代替 91。
   - env-debt2 账本尾部补合并事实行（「#304 已于 b23fe459
     合入；Windows localdeps Oracle 仍欠——见 R22 边界」）；
   - 「账本入库」纪律检查进现有门禁评估（能否脚本化：
     master 无对应账本的已合方向清单——R0 定案）。
3. **计数对齐**：AUDIT_ISSUES 头部 snapshot note 更新
  （或标注「快照日期」防再漂——倾向加日期戳）；TODOS:109
   SGS 半句对齐注记（与方向 91 协调）。
4. **lupdate 骨架**：跑 lupdate 刷 .ts（#311 遗留 + 各方向
   新增 tr()）——新增 unfinished 条目按 #246 术语表批量
   翻译或列清单记 TODOS（26K churn 的处理量级 R0 实测定）。
5. **漂移护栏**：轻量检查——`check_docs_drift.py`（或并入
   既有脚本），三项静态可查，挂 lint Source gates：
   ① 读取 `goal-loop-prompts/README.md` 写明的
   `编号任务书份数：<整数>`，与目录中编号任务书实数比较
   （`^[0-9]+-.*\.md`，不含 `README.md`）。二者不等即红。
   **禁止把期望值写死成 100，也禁止写死成 94**——期望值只
   来自 README 那一行。本校正时实数是 94（缺 85–90；
   目录 md 共 95，含 README）。写死 100 会在落地当天就红，
   而且会逼人补造不存在的任务书。不要用下表行数冒充目录
   实数（下表没有 36–47、73/74）。
   ② UI_LAYER_PLAN 的行数落点 vs 实测（±20% 容差）；
   ③ AUDIT_ISSUES 头部计数带日期戳。
6. **测试**：护栏 mutation（故意改错计数打红）；lupdate 后
   check_i18n/check_l10n 绿。

## 通用纪律（方向内全程有效）

- **分层**：文档 + lupdate + 轻脚本；src/ 零改动（账本摘取
  是文档文件）；`check_layering.py --strict` 绿。
- **诚实面**：#310 账本原文保留，档案标题写明是 #310，
  不把 1H/5M/7L 当成 #308 证据；#308 交付说明只引用
  82ea9cd9。漂移护栏的容差口径记 ledger；份数检查不写死
  100。
- **资源**：`./paleo-dev.ps1` 系（lupdate 工具链）。
- **无人值守**：护栏项与容差自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-docs-drift.md`。
- **多轮 review（硬要求）**：每批 → 护栏绿 → diff 自审
  （事实核对/档案完整/计数准确/护栏有效/无越权 五维）→
  修复 → 再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 架构文档：UI_LAYER_PLAN W8 段与实况一致（数字抽检）；
   PALEO_QGIS_PLAN 无矛盾表述（rg 核对）。
2. 账本收编：`.goal-loop-ledger-sf-geostat.md` 以 #310
   账本档案入库（原文保留；1H/5M/7L 不得充当 #308 证据）；
   另有一份只依据 `82ea9cd9` 的 #308 交付说明；env-debt2
   合并事实行；「MAPPING_WORKBENCH 不存在」以 #310 勘察
   错误注记，不删原文、不改记到 #308。
3. 计数：AUDIT_ISSUES 带日期戳；TODOS:109 注记（或 91 已
   修的确认）。
4. lupdate：.ts 刷新；check_i18n/check_l10n 绿；新增
   unfinished 处置（翻译或清单）有记录。
5. 护栏：check_docs_drift 三项静态检查进 Source gates；
   mutation 打红。份数检查的期望值来自 README 的
   `编号任务书份数：` 行：把该行改成 100、或在脚本里写死
   100，都必须红。当前目录实数是 94，README 写 94 时护栏绿。
6. 全量 ctest 对照 R0 红集合 diff 为空（若跑了构建）。
