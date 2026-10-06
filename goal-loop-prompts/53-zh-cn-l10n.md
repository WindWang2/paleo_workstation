# Goal-Loop 方向 53：zh_CN 本地化落地——5,196 条翻译清零 + lrelease 产物门禁

## 背景（实测事实，勿再勘察；2026-10-05 master `adf2be7` 口径）

- `translations/paleo_zh_CN.ts`：**5,196 条 message，5,196 条
  unfinished（100% 未译）**——lupdate 骨架已建（随 fcc2d34 重跑），
  译文未填。这是当前用户可见度最高的缺口（整站英文界面）。
- `tr()` 覆盖：src 全域 5,661 次（ui 4,289 / services 121）——
  入口已全覆盖，check_i18n.py 实跑 exit 0（文案入口无裸字面量）。
- 门禁现状：tools/check_i18n.py 只挡新增回归（词表外模式不扫）；
  **无 lrelease 产物门禁**——.ts 无 .qm 产物链路，运行时翻译
  加载未接线。
- 仓库语言事实：代码注释/commit/文档均中文语境，UI 文案英文
  先行——翻译是补齐不是改向。

翻译口径（必须遵守，防机械直译灾难）：

1. 地质专业词按行业惯例：horizon=层位、fault=断层、well top=
   井分层（分层点）、crossplot=交会图、gridding=网格化、
   sequence framework=层序格架、realization=实现（多个实现）、
   facies=相/沉积相、property modeling=属性建模、seismic
   section=地震剖面、time-depth=时深（转换）。
2. UI 控件短文案按桌面软件惯例（OK/取消/应用/确定；Dock=
   停靠窗口）；Qt 标准对话翻译与 Qt 自带 zh_CN 术语一致。
3. 疑难条目宁留原文不硬译；占位符 `%1`/`%n` 保持原样。

## 环境接线（Windows 本机实测口径，源 goal/sf-kriging 账本 R0）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\zh-cn-l10n -b goal/zh-cn-l10n-20261006 origin/master
cd .worktrees\zh-cn-l10n
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DQGIS_PREFIX=C:/Users/wangj.KEVIN/paleo-qgis-prefix `
  -DCMAKE_PREFIX_PATH="C:/deps/Qt/6.8.0/msvc2022_64;C:/Users/wangj.KEVIN/paleo-qgis-deps/Library;C:/deps/qscintilla-install" `
  -DQSCINTILLA_PREFIX=C:/deps/qscintilla-install
cmake --build build -j8
```

lupdate/lrelease 走 Qt 工具链（`C:/deps/Qt/6.8.0/msvc2022_64/bin`
或 paleo-qgis-deps 内；R0 先探测哪个前缀有 lrelease 并记档）。
不需要改 C++ 代码主体（tr() 已全覆盖）——只动 translations/、
构建接线、加载点。

## 预算（开发量）

- **Agent tokens**：上限 3 亿，预计 1.0–1.6 亿（5,196 条翻译是
  大体量生成任务 + 术语表建设 + 门禁脚本）。
- **执行花费**：8–12 轮（构建轮次少——代码面小），预计墙钟
  1–2 个工作日会话；翻译生成为主要 token 消耗。

## 目标形态（建议按序）

1. **术语表先行**：`translations/glossary-zh-CN.md`——按 docs/
   progress 目录名域（seismic/well/mapping/modeling…）整理地质
   术语中英对照表（≥120 词），疑难条目裁决记档；后续批次翻译
   以此为准。
2. **分批翻译**：.ts 按 context 分批（每批 ≤600 条）填译文；
   占位符/HTML 标签保持；finished 状态由 lupdate 复核。
3. **lrelease 产物链**：CMake 加 translations 目标（lrelease 编译
   .qm 进 build；安装/资源部署按仓内先例——若仓内无先例，
  qm 嵌入 .qrc 资源随二进制走，记决策）。
4. **运行时加载**：app 启动按 QLocale 装 zh_CN .qm（先例探测：
   main.cpp 现有翻译加载代码若已有则只补产物，勿重复造）。
5. **门禁**：check_i18n.py 加「.ts 存在则 unfinished 必须为 0」
   档（或新增 check_l10n.py）；.qm 产物存在性断言。
6. **抽样验证**：offscreen 启动 + QLocale zh_CN，截图 3–5 个
   代表页面（主窗/数据页/剖面 dock）验证翻译实际生效，入 ledger
   （截图存 docs/progress/l10n-shots/ 或仓内既有截图目录先例）。

## 通用纪律（方向内全程有效）

- **分层**：只动 translations/、CMake、main 加载点与 tools 门禁；
  src/ 代码原则上零改动（若 tr() 上下文缺陷导致翻译错位，最小
  修复并记档）。
- **术语一致**：同一词全文一致（层位不混「地平线」）；Qt 标准
  短语与 Qt zh_CN 官方翻译一致。
- **资源**：构建 `-j8`；本方向构建轮次少。
- **无人值守**：疑难术语自行按行业惯例裁决并记 glossary，不等
  人工确认。
- **ledger**：`.goal-loop-ledger-zh-cn-l10n.md`。
- **多轮 review（硬要求）**：每批 → lrelease 零 error → 抽检
   译文质量（术语一致/占位符完整/context 错位）→ 修复 → 再
   review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. lrelease 全量编译零 error；`unfinished` 计数 0（脚本断言输出）。
2. 运行时加载：zh_CN locale 启动日志/截图证据≥3 页面呈中文；
   en locale 回退英文不受影响。
3. 门禁：新增 l10n 检查项进 ctest（或既有 check_i18n 扩档），
   故意留一条 unfinished 能打红（mutation 验证）。
4. 术语抽检：glossary ≥120 词；随机抽 40 条译文与 glossary
   零冲突（脚本或人工抽检记录）。
5. 占位符完整：脚本断言译文含与源文相同的 %n 占位符集合
  （数量与编号一致）。
6. 回归：全量 ctest 无新增失败；check_i18n.py 既有口径仍绿。
