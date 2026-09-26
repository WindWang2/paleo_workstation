你在 paleo_workstation 项目实现「运行时韧性」包：崩溃报告机制（本地转储 + 重启恢复提示）与外链源文件「重新定位」恢复路径（对应 docs/PROJECT_AREA_PLAN.md §38 与 TODOS.md autoplan pass-2 递延项——由编排会话批准提前实施）。

【执行方式】
- 预算约 3 亿 tokens。自主推进到【验收 Oracle】全部通过，不中途请示；卡住时换方法而不是退出。
- 单线程串行实现。**禁止使用** workflow / teamwork / swarm / 并行子代理编排（不 spawn_subagent、不开 worker）。所有代码你自己写。
- 按 goal-loop 协议执行（若宿主有 /goal-loop 或 /goal skill 则调用之）：
  - 开始前声明一次：目标一句话、完成条件 Oracle（=下方【验收】全部通过）、迭代上限 40 轮
  - 在工作区根维护 `.goal-loop-ledger.md` 账本；每轮只做一处聚焦改动，亲自跑验证命令，记账 `第N轮 | 改动 | 验证结果 | 通过/未通过 | 下一步`
  - 验收未满足禁止宣告完成；完成后关键验证连跑两遍
- TDD：每个交付物先写 QTest 失败测试（红），再实现到通过（绿）。测试文件用 `add_paleo_test()` 注册。
- 用 /qa、/review 类只读 skill 做自查可以；不要用它们衍生别的执行模式。

【环境】
- 仓库 /home/kevin/projects/paleo_workstation，C++20，Qt 6.11.2，QGIS 4.2.2 系统安装（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui，moc=/usr/lib/qt6/moc）。系统 QGIS 已装，勿 vendor。
- 构建：`ninja -C build`；测试：`cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure`。
- 权威契约：`docs/PROJECT_AREA_PLAN.md`；架构边界 `docs/PALEO_QGIS_PLAN.md`（§38 崩溃报告决策留白）；视觉 `DESIGN.md`（文案/胶囊走 token）；不做清单 `TODOS.md`。
- 先跑 gstack 自检：`_GS=""; for _D in "${GSTACK_ROOT:-}" "$HOME/.claude/skills/gstack" "$HOME/.grok/skills/gstack"; do [ -z "$_GS" ] && [ -n "$_D" ] && [ -d "$_D/bin" ] && _GS="$_D"; done; [ -n "$_GS" ] && echo GSTACK_OK || echo GSTACK_MISSING`
- 验收数据源：`/home/kevin/projects/paleo_project/data/project_area`。禁止把大文件提交进仓库；夹具放 `testdata/`。

【第一步：建 worktree】
```bash
cd /home/kevin/projects/paleo_workstation
git fetch origin && git worktree add ../pw-runtime-resilience -b wave4/runtime-resilience origin/master
cd ../pw-runtime-resilience   # 之后所有工作在此 worktree 内进行
cp build/CMakeCache.txt build-cache-seed.txt 2>/dev/null; mkdir -p build && cd build && cmake .. && cd ..
```

【并行会话的接缝——重要】
另一个 agent 与本编排会话并行，文件面划分如下，**你越界=合并冲突**：
- 你拥有：`src/app/main.cpp`、`src/app/appcontext.*`（崩溃报告接线）、`src/ui/datapreview/**`（重定位入口/死胡同按钮）、`src/io/dataimportservice.*` **仅限新增**「外链重定位 API」块（放在文件末尾新 section，别动既有函数体）、新增 `src/services/crashreport.*`（或等价新文件）、`src/metadata/**` 最小触碰（若需记录 crash flag 放工程侧写处）、tests/ 里本域测试、新增 docs 文件。
- 不碰：`src/io/projectclassifier.*`、`isKnownSequenceBoundary`/层位名单（B 包外置中）、`src/workflow/**`、`src/catalog/**`、`src/ui/pages/**`、`src/ui/paleotheme.*`。
- **灰区**：`dataimportservice.cpp` 与 B 包共文件——你只加新函数/新 API，不改既有函数体；B 只把 `isKnownSequenceBoundary` 的常量表外置。两处改动区域天然不相交。
- `CMakeLists.txt`：新增放**一个连续块**并注释 `# wave4/runtime-resilience`；合并冲突时保留双方行。
- 不要更新 `docs/PROJECT_AREA_PLAN.md` 与 `TODOS.md` 的任务勾选——文档由编排会话统一对账；你的完成项写进 PR body。

【任务背景】
- `DataCatalog`：版本有 `sourceUri`/`managed`/`path`；`versionBySha256` 已要求文件仍在且重哈希一致（受管丢失的旧条目不冒充命中）；`absolutePathForVersion`/`resolvedVersionPath` 是权威路径解析。
- `DataImportService`：`importProjectFileEx` 走 `catInvoke` marshal 回 GUI 线程；外链版本 `managed=false` 时 `absolutePath` 直接回 `v.path`（=源文件绝对路径，移动后失效）；preview 侧「找不到源文件」错误文案在 `dataimportservice.cpp` L303 一带。
- 崩溃报告：PALEO_QGIS_PLAN §38 留了「本地转储 vs 回传」决策——本包按**本地优先**落地：不联网、不回传；崩溃时写本地报告文件 + 重启检测提示。

【交付物】

1. **崩溃报告机制（本地转储）**
   - 新组件（建议 `src/services/crashreport.{h,cpp}` 或 `src/app/` 下）：`installCrashHandler()` 在 `main()` 启动早期挂上 SIGSEGV/SIGABRT/SIGFPE 等致命信号处理器；崩溃时异步信号安全地写 `<工程目录或 QStandardPaths::AppDataLocation>/crash/YYYYmmdd-HHMMSS.txt`：时间戳、应用版本、QGIS/Qt 版本、当前工程路径、（可用时）`backtrace()`/`backtrace_symbols_fd` 栈帧。
   - 「上次未干净退出」检测：启动写 `.running` flag、正常退出清除；下次启动 flag 仍在 → 上次崩溃 → 主窗状态栏或启动页给诚实提示（含最近一份 crash 报告路径），文案走 DESIGN.md。
   - Windows/无 execinfo 平台编译安全（`#ifdef` 降级为仅 flag+时间戳报告）。
   - 测试：报告文件格式、flag 生命周期（脏退出检测）、重启提示露出——handler 本身不测（fork 子进程触发信号的用例如可控可加，不稳定则不要）。
   - 文档：在 `docs/` 落 `CRASH_REPORTING.md`（或在 SCHEMA_MIGRATION 同级单文件）：决策记录（本地转储、不回传、隐私边界）、文件格式、恢复流程。

2. **外链源文件「重新定位」恢复路径**
   - 场景：RAW 外链版本（`managed=false`）源文件被移动/重命名 → 预览/打开报「找不到源文件」死胡同。
   - 服务层：`DataImportService`（或 catalog 接缝处）新增 `relocateVersionSource(versionId, newPath, error)`：流式 SHA-256 复验新文件 → 与原版本 sha 一致 → 更新版本 `sourceUri`/`path` 并保存（复用既有校验面）；不一致 → 拒解，报错写明「文件内容与原版本不符」（不静默换源）。处理新路径在工程目录内的情形（仍按 external 记，不升级为 managed）。
   - UI 层：预览标签在「找不到源文件」错误面上给「重新定位文件…」按钮 → `QFileDialog::getOpenFileName` → 调 relink API → 成功重载预览；失败如实报错。未决/死胡同文案已有 `deadEndText` 惯例可循。
   - 测试：移动源文件后 relink 成功路径、SHA 不匹配拒解、catalog 持久化后路径存活、UI 按钮存在性（offscreen）。

【验收 Oracle】（全部满足才算完成）
- `ninja -C build` 零错误；`QT_QPA_PLATFORM=offscreen ctest --output-on-failure` 全绿（含新增测试），连跑两遍。
- 新测试覆盖：crash flag 脏退出检测、报告文件生成/格式、relink 成功/SHA 拒绝/重启存活、UI 入口存在。
- `docs/CRASH_REPORTING.md` 落盘且与实现一致（引用处有代码/测试指针）。
- 若 `PALEO_REAL_PROJECT_AREA` 可用：真数据回归不劣化。
- Windows 构建路径不因新代码破坏（`#ifdef` 覆盖；本地无 MSVC 时至少保证 `#ifndef Q_OS_WIN` 分支编译通过且不引入 POSIX-only 头到公共路径）。

【纪律】
- 不碰清单见【接缝】。不做：崩溃回传联网、第三方 breakpad/crashpad 依赖引入、重新扫描 catalog 全量校验、第二工区参数化（B 包）。
- 提交：worktree 分支上按仓库惯例 commit（小步、说 why）。完成且验收绿后：
  ```bash
  git push -u origin wave4/runtime-resilience
  gh pr create --title "wave4: crash reporting + external-source relocation" --body "$(cat <<'EOF'
  ## Summary
  - <逐项：crash dump + dirty-flag 恢复提示 / relocateVersionSource + UI 入口>
  #### Test plan
  - [ ] ninja -C build clean; ctest 全绿 ×2
  EOF
  )"
  ```
  若 push/gh 不可用，报告分支名与 commit 列表，不要强行处理。
- 最终报告：测试通过数、改动文件清单、文档路径、给集成者的冲突提示（特别是 dataimportservice.cpp 与 B 包的重叠区）。
