你在 paleo_workstation 项目实现「UX 一致性收尾」包：状态胶囊、空态与未决路径、字体/焦点环/a11y、资产操作按链接身份重键+undo 恢复、「在地图上显示」双向同步、catalog 打开失败的状态栏露出、渲染测试稳定化、WebEngine 可选化（对应 docs/PROJECT_AREA_PLAN.md pass-2 批准的 T27/T28/T29/T31/T32、T20 余项、D14）。

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
- 权威契约：`docs/PROJECT_AREA_PLAN.md`（pass-2 报告 + T 任务清单）；**视觉唯一权威 `DESIGN.md`——动手前通读，所有颜色/字号/间距/胶囊样式以它为准，AGENTS.md 明文要求**；架构 `docs/PALEO_QGIS_PLAN.md`；不做清单 `TODOS.md`。
- 先跑 gstack 自检：`_GS=""; for _D in "${GSTACK_ROOT:-}" "$HOME/.claude/skills/gstack" "$HOME/.grok/skills/gstack"; do [ -z "$_GS" ] && [ -n "$_D" ] && [ -d "$_D/bin" ] && _GS="$_D"; done; [ -n "$_GS" ] && echo GSTACK_OK || echo GSTACK_MISSING`
- 验收数据源：`/home/kevin/projects/paleo_project/data/project_area`。**禁止**把大数据提交进仓库；夹具放 `testdata/`。

【第一步：建 worktree】
```bash
cd /home/kevin/projects/paleo_workstation
git fetch origin && git worktree add ../pw-ux-consistency -b wave3/ux-consistency origin/master
cd ../pw-ux-consistency   # 之后所有工作在此 worktree 内进行
mkdir -p build && cd build && cmake .. && cd ..
```

【并行会话的接缝——重要】
- 你拥有：`src/ui/**`（pagepanels、datapreview、paleomainwindow、webviewpanel、statusbar、释放/属性面板等）、`tests/` 里 UI 域测试（tst_ui/tst_panels/tst_datapreview/tst_threeway 等）、字体资源与 qrc、渲染对比测试基建。
- 不碰：`src/catalog/**`、`src/io/**`（A 包）、`src/workflow/**`（B 包）——T28 里你只改 UI 层调用方式，catalog 的 `linksForEntity/unresolvedLinks/setPrimary` 等 API 已存在，直接用，**不改签名**。
- **灰区**：`src/services/appcontext.*` 若必须动（如状态栏信号），最小改动并在 PR 点名。`paleomainwindow.cpp`/`pagepanels.cpp` 本会话也在改 Wave-2 的 UX 扩张——**你严格不做**：井上图层（D6）、预览高度/最大化（D7）、厚度触发按钮（D8）、GeoJSON 临时配准（D11）、异步任务页/进度条（D1/D2）。你的增量按「每条交付物一个连续代码块」组织，减少未来 rebase 冲突。
- `CMakeLists.txt`：三方都会加东西——你的新增放**一个连续块**并注释 `# wave3/ux-consistency`。
- 不要更新 `docs/PROJECT_AREA_PLAN.md` 与 `TODOS.md` 的任务勾选——文档由编排会话统一对账；完成项写进 PR body。

【任务背景】
- DESIGN.md 已定 token：状态胶囊（未决胶囊已在用）、Noto Sans SC 正文 + JetBrains Mono 数字 9pt、2px `#1B73D0` 焦点环——但字体从未 vendor/注册、焦点环未实现、多数状态文字仍是 `setForeground` 彩色文本（对比度 2.3–3.3:1 不达标）。
- 资产表的「关联」列与未决徽标已有；attach/primary 动作现按 `links()` 下标寻址（行序变即指错）；undo 只在会话内，降级 primary 与 note 不恢复。
- 「在地图上显示」现只单向触发；catalog 打开失败发了 `catalogOpenFailed` 信号但没有 UI 汇点。
- 渲染对比测试跨平台噪点：需 vendor 字体 + 钉光栅化。

【交付物】

1. **T27 — 状态标签胶囊化 + 文案中文化**
   - 资产/版本/实体表里的状态文字全部改为 DESIGN.md 胶囊样式（背景 token + 圆角 + 居中），禁用 `setForeground` 彩色裸文字；新增中性「未计算」胶囊用于无栅格/无版本状态。
   - `coordinate_status` 枚举的显示串中文化：「坐标无效」「没有坐标」等（与 plan 文案对齐）；验证页其余英文状态消息一并中文化。

2. **T31 — 空态与未决死胡同**
   - 资产表空、地图无图层、图层树空三类空态：居中提示文案 + 下一步动作指引（如「先导入工区文件夹」）。
   - 未决资产的多井 tab：无井可挂时显示死胡同文案 + 指向「挂到这口井」入口的说明，不留空白页。
   - 文件夹确认对话框完成后提供「查看未决」过滤动作（把资产表过滤到未决行）。

3. **T32 — 字体注册 + 焦点环 + Mono 数字面 + a11y + tab 溢出**
   - vendor Noto Sans SC + JetBrains Mono（放 `resources/fonts/` 并 qrc 或安装注册，`QFontDatabase::addApplicationFont`），应用启动时注册；找不到字体时在日志明说降级。
   - 全局 2px `#1B73D0` 焦点环（stylesheet 或 palette，覆盖 QLineEdit/QComboBox/QTableView/QPushButton 等可聚焦件）。
   - 所有数字面（表格数值列、坐标、计数）用 JetBrains Mono 9pt。
   - `accessibleName`/`accessibleDescription` 覆盖：ReleasePanel、AttributeTablePanel、各导入按钮、文件夹确认表。
   - tab 溢出策略：tab 超过可用宽时启用滚动而非挤压（Qt 自带 `usesScrollButtons`，确认并钉行为）。

4. **T28 — attach/primary 动作按链接身份重键 + undo 恢复**
   - 「关联/设为主数据/解除」等动作按 `(assetId, role)` 链接身份寻址，不再按 `links()` 行下标；`changed()` 信号刷新后确认条仍存活。
   - undo 恢复被降级的 primary 与 note，且**跨 reload 持久**（undo 栈随工程保存/恢复的最小实现，或按批准记录实现的等价方案——D4 已定「恢复降级 primary + 保留 note」）。
   - 测试：行重排后动作仍命中正确链接；undo 跨 `open()` 后恢复 primary+note。

5. **T29 — 「在地图上显示」双向同步**
   - 图层可见性 ↔ 按钮/勾选状态双向同步（图层树隐藏资产图层时按钮态跟随）；点击时在地图上 zoom 到该资产图层并闪烁定位 ~300–500ms。
   - 测试：隐藏图层后按钮状态断言、点击后 canvas 范围与闪烁计时器断言。

6. **T20 余项 — `catalogOpenFailed` 状态栏露出**
   - 连接该信号到主窗口状态栏：常驻告警文案（红胶囊样式，DESIGN.md token），含打开失败原因；恢复前禁用「导入」类动作（若已有禁用逻辑则复用）。
   - 测试：注入打开失败→状态栏文案断言 + 导入按钮禁用断言。

7. **渲染对比测试稳定化**（TODOS P1）
   - 测试启动时装 vendor 字体（与交付物 3 共用注册代码路径）、钉 `QT_WIDGETS` 光栅化参数，消除跨平台字体噪点导致的假失败。

8. **D14 — `Qt6 WebEngineWidgets` 改可选构建依赖**
   - `find_package(Qt6 COMPONENTS WebEngineWidgets QUIET)` + 编译宏；缺库时 `WebViewPanel` 走现有降级路径（外部浏览器兜底已实现），构建不失败。
   - 验证：`cmake -D...` 无 WebEngine 配置的构建通过（若本机有库，用宏路径编一遍即可，测试断言降级分支可达）。

【验收 Oracle】（全部满足才算完成）
- `ninja -C build` 零错误；`QT_QPA_PLATFORM=offscreen ctest --output-on-failure` 全绿（含新增测试），连跑两遍。
- 新测试覆盖：胶囊渲染（颜色 token 断言或截图基线）、空态文案、未决 tab 死胡同、查看未决过滤、字体注册（`QFontDatabase` 查询断言）、焦点环（样式表/palette 断言）、Mono 数字面、a11y 名称、链接身份寻址、undo 跨 reload、地图双向同步、状态栏 catalog 失败露出、WebEngine 降级构建路径。
- DESIGN.md 符合性：不引入 token 外颜色/字号。
- 若 `PALEO_REAL_PROJECT_AREA` 可用：真数据 smoke 不劣化。

【纪律】
- 不碰清单见【接缝】：`src/catalog/io/workflow` 禁碰；不做 D6/D7/D8/D11/D1/D2（本会话范围）；不改 catalog API。
- 不做：暗色模式、简化版 composer、tab 重排/拖拽、任何 TODOS 触发条件未到的项。
- 提交：worktree 分支上按仓库惯例 commit（小步、说 why）。完成且验收绿后：
  ```bash
  git push -u origin wave3/ux-consistency
  gh pr create --title "wave3: UX consistency (capsules/fonts/a11y/link-identity/undo)" --body "$(cat <<'EOF'
  ## Summary
  - <逐项：T27 / T31 / T32 / T28 / T29 / T20 余项 / render-test / D14>
  #### Test plan
  - [ ] ninja -C build clean; ctest 53+N/53+N green ×2
  EOF
  )"
  ```
  若 push/gh 不可用，报告分支名与 commit 列表，不要强行处理。
- 最终报告：测试通过数、改动文件清单、DESIGN.md 符合性自查、给集成者的冲突提示。
