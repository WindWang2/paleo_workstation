# Goal-Loop 方向 25：图件编制与出版输出——布局设计器补全

## 背景（实测事实，勿再勘察）

`src/ui/layoutdesignershell.h` 目前只是壳（E2 自研欠账之一）。vendored QGIS 提供
`QgsPrintLayout`/`QgsLayoutDesignerDialog`/`QgsLayoutExporter` 基础件；Paleo 侧负责
模板、元素默认、导出管线与工程持久化。**图件资产必须登记 catalog 版本体系，
不产游离文件。**

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/layout-publishing -b goal/layout-publishing-20261004 origin/master
cd .worktrees/layout-publishing
# vendor 三件为 gitignored——须从主仓绝对路径 symlink（../../ 相对路径会自环，勿用）
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序，每个可独立原子提交）

1. **版式模板库**：三类内置模板（井位图/单因素图/沉积相图，A4/A3 横竖）；模板作工程资产持久化，可另存/重命名/删除。
2. **标准图件元素**：比例尺（数字+条式）、图例（随图层树自动更新可删减）、指北针、坐标/经纬网格、图名+副题+署名文本块；元素属性面板走 `PaleoTheme` token。
3. **地图项绑定**：主地图+插图（全图位置指示），比例尺/范围联动与锁定；地图内容=当前图层树快照或实时（可选）。
4. **导出链路**：PNG(150/300/600dpi)/PDF/SVG；批量导出——按层位组一键每层一幅，文件名规则化（工程_层位_日期）。
5. **版式持久化**：布局随工程保存/恢复（QGZ 布局 XML 入库或工程内文件），重开可继续编辑。
6. **设计器外壳补全**：元素树、属性面板、标尺/参考线、对齐吸附、页缩放；接入主导航非孤岛窗口。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；`tools/check_layering.py --strict` 必绿。
- **资源**：构建/测试一律 `-j8`。
- **内存所有权**：QGIS 布局对象生命周期是重灾区（layout/item 父子关系、SIP 语义在 C++ 里不豁免）——自审必查 item 所有权与悬空指针。
- **UI**：视觉决策对照 `DESIGN.md`；i18n 过 `i18n`/`i18n_selftest`。
- **性能断言**：禁绝对毫秒墙钟。
- **ledger**：`.goal-loop-ledger-layout-publishing.md` 记「改动/验证/判定/下一步」。
- **多轮 review（硬要求）**：每批 → 构建+测试全绿 → diff 全量自审（分层/所有权/导出与画布像素一致性/i18n/DESIGN.md/游离文件六维）→ 修复 → 再 review，**至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 模板 round-trip：新建-保存-重开后元素树/绑定/属性逐字段一致。
2. 导出与画布同图层渲染（可像素抽样断言）；批量导出计数=层位组数。
3. 布局资产入 catalog 有正常版本/SHA。
4. 全量绿：新导出用例 + `tst_ui`、`tst_previewdoc`；`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（`## Summary`/`#### Test plan`/自审结论清单/遗留项）。**不合并、不推 master、不等 CI。**
