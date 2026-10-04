# Goal-Loop 方向 29：UI 视觉一致性与打磨收口（第二轮）

## 背景（实测事实，勿再勘察）

第一批 `01-ui-experience-polish` 已落地过一轮打磨；此后又并入 wellsection、
datapreviewtabs 拆分、well-facies 面板、jobrunner 等新面，视觉一致性重新漂移。
**`DESIGN.md` 是唯一准绳**——任何 token（颜色/字号/间距/圆角/层级）不得自创，
`PaleoTheme::tokens()` 与 `PaleoIcons` 是唯一来源。本方向只打磨观感，**不改 UX
结构与文案语义**（动文案须同时过 i18n 且属 High 级审慎项）。

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/ui-visual-polish -b goal/ui-visual-polish-20261004 origin/master
cd .worktrees/ui-visual-polish
# vendor 三件为 gitignored——须从主仓绝对路径 symlink（../../ 相对路径会自环，勿用）
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序，每批可独立原子提交）

1. **token 违例普查**：`rg` 扫 `src/ui` 硬编码 `#hex`/pointSize/像素 padding/margin —— 归一 `PaleoTheme::tokens()`；确需例外的加注释说明，输出违例清零清单。
2. **图标体系收口**：动作图标全量走 `PaleoIcons::qgisTheme`/主题 SVG；统一尺寸/描边/留白规范；缺失图标补制并按 contract test（`tst_layertreepanel` 图标契约用例，`a0d27c5` 已落地）登记。
3. **面板一致性**：数据/预测/约束/单因素/编图/验证 + wellsection + datapreview 各页——卡片圆角、分组标题层级、内外间距、分割线统一；列表/表单行高对齐。
4. **状态视觉规范**：空态/加载/错误/禁用四态样式收口（`stateLabel`/`loadingLabel` 样板统一），各页补齐缺失态而非留空白页。
5. **主题与对比度**：明暗主题切换完整性走查；正文/次级文字/禁用态对比度抽检；地图符号与预览图表在双主题下可读。
6. **截图对照档案**：关键页 before/after 截图逐张入 ledger（含 HiDPI 抽查），供人审复核。

## 通用纪律（方向内全程有效）

- **只动观感**：不改信号槽接线、不改交互流程、不改文案语义；结构性需求（如重排布局层级）列为遗留项不擅自做。
- **分层**：`src/` 新文件头三行 `// 层：<词表>`；样式代码归 `src/ui` 面，禁漏进数据/功能层。
- **资源**：构建/测试一律 `-j8`。
- **vendor**：改 vendor 件登记 `PATCHES.md`；QGIS 主题 SVG 增补走 `PaleoIcons` 注册面。
- **i18n**：新文案必过 `i18n`/`i18n_selftest`；样式字符串不入翻译面。
- **ledger**：`.goal-loop-ledger-ui-visual-polish.md`，**每处视觉改动附前后截图对照**（ledger 协议既定动作）。
- **多轮 review（硬要求）**：每批 → 构建+测试全绿 → diff 全量自审（token 偏离/结构改动混入/i18n/截图一致性/分层/死样式六维）→ 修复 → 再 review，**至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. `rg` token 违例扫描报告：前后计数对照，非白名单违例清零。
2. 截图档案：改动页 before/after 全齐，视觉上确无回退（无错位/截断/溢出）。
3. 图标契约测试原样绿；新增图标进契约清单。
4. 全量绿：`tst_ui`、`tst_layertreepanel`、`tst_datapreview`、`tst_previewmap_*`；`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（`## Summary`/`#### Test plan`/自审结论清单/遗留项 + 关键前后截图链接）。**不合并、不推 master、不等 CI。**
