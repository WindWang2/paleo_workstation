你在 paleo_workstation 项目实现「第二工区参数化接缝 + Phase-0 vendor 收口」包：把本工区钉死值外置为工程级配置 seam（分类器目录规则、8 层序界面名单、SEG-Y 索引约定、ONNX 网格门），并补齐 Phase-0 spike 交付物（vendor superbuild 骨架、平台矩阵、app-only 审计、TTHW 记录）。对应 TODOS.md「第二工区参数化接缝」（P3，由编排会话批准提前实施）与「Phase 0 spike 交付物」（§39/E3）。

【执行方式】
- 预算约 3 亿 tokens。自主推进到【验收 Oracle】全部通过，不中途请示；卡住时换方法而不是退出。
- 单线程串行实现。**禁止使用** workflow / teamwork / swarm / 并行子代理编排（不 spawn_subagent、不开 worker）。所有代码你自己写。
- 按 goal-loop 协议执行（若宿主有 /goal-loop 或 /goal skill 则调用之）：
  - 开始前声明一次：目标一句话、完成条件 Oracle（=下方【验收】全部通过）、迭代上限 40 轮
  - 在工作区根维护 `.goal-loop-ledger.md` 账本；每轮只做一处聚焦改动，亲自跑验证命令，记账 `第N轮 | 改动 | 验证结果 | 通过/未通过 | 下一步`
  - 验收未满足禁止宣告完成；完成后关键验证连跑两遍
- TDD：参数化 seam 的行为等价性用「默认配置 = 现行为」测试钉死（红→绿）；文档类交付物用引用核对（文档里写的函数/常量真实存在）。
- 用 /qa、/review 类只读 skill 做自查可以；不要用它们衍生别的执行模式。

【环境】
- 仓库 /home/kevin/projects/paleo_workstation，C++20，Qt 6.11.2，QGIS 4.2.2 系统安装（headers /usr/include/qgis，libs -lqgis_core -lqgis_gui，moc=/usr/lib/qt6/moc）。系统 QGIS 已装，勿 vendor。
- 构建：`ninja -C build`；测试：`cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure`。
- 权威契约：`docs/PROJECT_AREA_PLAN.md`；架构边界 `docs/PALEO_QGIS_PLAN.md` §39/E3；视觉 `DESIGN.md`；不做清单 `TODOS.md`。
- 先跑 gstack 自检：`_GS=""; for _D in "${GSTACK_ROOT:-}" "$HOME/.claude/skills/gstack" "$HOME/.grok/skills/gstack"; do [ -z "$_GS" ] && [ -n "$_D" ] && [ -d "$_D/bin" ] && _GS="$_D"; done; [ -n "$_GS" ] && echo GSTACK_OK || echo GSTACK_MISSING`
- 验收数据源：`/home/kevin/projects/paleo_project/data/project_area`。禁止把大文件提交进仓库。

【第一步：建 worktree】
```bash
cd /home/kevin/projects/paleo_workstation
git fetch origin && git worktree add ../pw-area-param -b wave4/area-parametrization origin/master
cd ../pw-area-param   # 之后所有工作在此 worktree 内进行
cp build/CMakeCache.txt build-cache-seed.txt 2>/dev/null; mkdir -p build && cd build && cmake .. && cd ..
```

【并行会话的接缝——重要】
另一个 agent 与本编排会话并行，文件面划分如下，**你越界=合并冲突**：
- 你拥有：`src/io/projectclassifier.*`、`src/io/arearules.*`（新文件，建议承载全部工程级参数）、`src/io/segyreader.*`（仅限把道号索引/inline 约定常量经 AreaRules 读出）、ONNX 网格尺寸门的读取点、`vendor/**`、`tools/**`、`BUILDING.md`、新增 docs、tests/ 里本域测试。
- 不碰：`src/app/main.cpp`、`src/app/appcontext.*`、`src/ui/**`（A 包）、`src/workflow/**`、`src/catalog/**`、`src/metadata/**`。
- **灰区**：`dataimportservice.cpp` 与 A 包共文件——你只把 `isKnownSequenceBoundary` 的硬编码表改为从 AreaRules 读（函数签名/调用点不动），A 在文件末尾加 relink API；两处不相交。
- `CMakeLists.txt`：新增放**一个连续块**并注释 `# wave4/area-parametrization`；合并冲突时保留双方行。
- 不要更新 `docs/PROJECT_AREA_PLAN.md` 与 `TODOS.md` 的任务勾选——文档由编排会话统一对账；你的完成项写进 PR body。

【任务背景】
- 本工区钉死值（TODOS「第二工区参数化接缝」原文枚举）：分类器中文目录名规则（`projectclassifier.cpp`：井位/井分层/时深/层位/参考资料等）、「8 层序界面」名单（`dataimportservice.cpp` 的 `isKnownSequenceBoundary`：C3/C6/D53/D61/D62/D63/D71/D72）、SEG-Y 按道号索引的 base 约定（field-record 字 = 冻结 inlineMin；`segyreader.cpp`/plan §7）、411×641 ONNX 门（`projectdata.h` 注释 + onnx 工作流断言处）。
- vendor 现状：`vendor/manifest.json` + `fetch-deps.sh` + `deb-closure.lock`（PR #12 已做 deb 锁定与 hash 校验）；尚无 superbuild 骨架与平台矩阵文档。

【交付物】

1. **AreaRules —— 工程级参数 seam（`src/io/arearules.{h,cpp}`）**
   - 统一承载四类钉死值：层序界面名单、分类器目录规则（中文目录名 → 类型/角色的映射表）、SEG-Y 索引约定（inline 字段来源：道号序 + field-record base；cdp 偏移 20）、ONNX 期望网格（411×641）。
   - 读取顺序：工程目录下 `project_area.json`（或等价约定名）→ 内置默认值（=当前本工区值）。缺文件/缺键 = 默认，行为与今天逐字节一致；坏 JSON 如实报错拒用（不静默回退）。
   - `projectclassifier.cpp` 的目录规则表与 `isKnownSequenceBoundary` 改为从 AreaRules 取——调用点签名不变。
   - 文档：`docs/AREA_PARAMETERS.md`——「每测区参数 vs 全局规则」分界表、配置 schema、新增工区的操作步骤。
   - 测试：默认值等价（现有测试全绿即证）、自定义名单生效、坏 JSON 报错。

2. **Phase-0 spike 收口（§39/E3 剩余项）**
   - `vendor/superbuild/` 骨架：`ExternalProject` 路线的 CMake 骨架或脚本占位 + README 说明何时启用（distro 包不满足时），不要求能完整构建——骨架 + 决策记录即可。
   - `docs/PLATFORM_MATRIX.md`：目标平台矩阵钉定（OS × 架构 × Qt/QGIS 来源），当前实测行如实标注（本机 Arch Linux + 系统 QGIS 4.2.2 + Windows CI 经 OSGeo4W）。
   - `docs/APP_ONLY_AUDIT.md`：app-only 功能审计清单——`src/app` vs `paleo_core`/`qgis_gui` 逐能力标注「必须在 app 层 / 可下沉」。
   - `BUILDING.md`：补 TTHW 目标（vendor 引导后首次绿色测试 2–5 分钟）与 QGIS_PREFIX_PATH/vendor 路径说明。

【验收 Oracle】（全部满足才算完成）
- `ninja -C build` 零错误；`QT_QPA_PLATFORM=offscreen ctest --output-on-failure` 全绿（含新增测试），连跑两遍。
- AreaRules：默认配置下全套既有测试不劣化（行为等价即证据）；新测试覆盖自定义配置生效 + 坏 JSON 拒用。
- 四文档落盘（AREA_PARAMETERS / PLATFORM_MATRIX / APP_ONLY_AUDIT + BUILDING.md 增补），引用核对通过。
- 若 `PALEO_REAL_PROJECT_AREA` 可用：真数据回归不劣化（参数化后真工区结果逐字节一致）。

【纪律】
- 不碰清单见【接缝】。不做：接入真实的第二工区数据（只做 seam）、UI 参数编辑界面、崩溃报告（A 包）、把参数暴露给 QSettings 之外的运行时通道。
- 提交：worktree 分支上按仓库惯例 commit（小步、说 why）。完成且验收绿后：
  ```bash
  git push -u origin wave4/area-parametrization
  gh pr create --title "wave4: per-area parameter seam + phase-0 vendor closeout" --body "$(cat <<'EOF'
  ## Summary
  - <逐项：AreaRules seam / superbuild 骨架 / 平台矩阵 / app-only 审计 / BUILDING.md>
  #### Test plan
  - [ ] ninja -C build clean; ctest 全绿 ×2; 真数据回归一致
  EOF
  )"
  ```
  若 push/gh 不可用，报告分支名与 commit 列表，不要强行处理。
- 最终报告：测试通过数、改动文件清单、文档路径、给集成者的冲突提示（特别是 dataimportservice.cpp 与 A 包的重叠区）。
