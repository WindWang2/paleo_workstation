# Goal-Loop 方向 31：地质符号库与花纹体系——岩性/相花纹、井符号与图例自动生成

## 背景（实测事实，勿再勘察）

图件专业化卡在符号体系：`QgisStyleService` 已有语义样式入口
（`applyBoundaryLayerStyle`/`applyConstraintLayerStyle`/`applyContourLayerStyle`/
`applyFaciesBoundaryStyle`/`applyWellCategoryStyle` 等逐类挂样式），但无行业
标准花纹库、无图例/选择器面。沉积相/岩性花纹、井别符号、断层与约束
线型都属编图出版刚需。**样式入口统一走 `QgisStyleService`（`src/qgis` 层），
UI 不直接堆 Qgs 符号代码；花纹资源走 qrc 或工程资产，禁散落绝对路径。**

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/geological-symbols -b goal/geological-symbols-20261004 origin/master
cd .worktrees/geological-symbols
# vendor 三件为 gitignored——须从主仓绝对路径 symlink（../../ 相对路径会自环，勿用）
ln -s /home/kevin/projects/paleo_workstation/vendor/superbuild/prefix vendor/superbuild/prefix
ln -s /home/kevin/projects/paleo_workstation/vendor/onnxruntime vendor/onnxruntime
[ -d /home/kevin/projects/paleo_workstation/vendor/prefix ] && \
  ln -s /home/kevin/projects/paleo_workstation/vendor/prefix vendor/prefix
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
cmake --build build -j8
```

## 目标形态（建议按序）

1. **岩性/沉积相花纹库**：常用花纹集（砂岩/泥岩/灰岩/砾岩/膏盐 + 常见相单元）——SVG pattern fill + line pattern 实现，资源进 qrc；花纹↔语义代码映射表单点维护。
2. **井符号体系**：井别符号（探井/开发井/注水井/报废井等）按 catalog 实体属性自动选型；符号缩放随比例尺。
3. **线型规范库**：断层（正/逆/走滑/推测）、相界（确定/推测/渐变相带）、测区边界——线宽/虚线式/端头规范统一进 `QgisStyleService`。
4. **图例自动生成**：按图层符号语义自动生成图例项（花纹 swatch + 名称），供方向 25 布局图例元素复用；图层增删联动刷新。
5. **符号选择器**：人工覆盖默认样式时的花纹/符号/线型浏览面板（预览缩略+语义过滤）。
6. **样式版本语义**：符号表随工程持久化，跨版本稳定（SVG 资源走 qrc 版本锚定，不引机器绝对路径）。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；符号语义/映射表归 `src/qgis` 或数据层，面板归 `src/ui`。
- **DESIGN.md**：符号视觉仍受设计系统管——自造花纹前先查 DESIGN.md 图例页（无则按行业惯例并在 ledger 注出处）。
- **资源**：构建/测试一律 `-j8`。
- **vendor**：改 vendor 件登记 `PATCHES.md`（QGIS 主题 SVG 增补同口径）。
- **i18n**：符号名/花纹名走翻译面。
- **性能断言**：花纹渲染不引入逐要素 QImage 重建——抽样渲染预算走比率门。
- **ledger**：`.goal-loop-ledger-geological-symbols.md`（符号视觉改动附截图对照）。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/样式收口单点性/资源打包/语义映射完整/i18n/DESIGN.md 六维）→ 修复 → 再 review，**至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 花纹 round-trip：图层挂花纹→工程保存重开→符号结构逐字段一致（SVG path/尺寸/旋转）。
2. 井别自动选型：合成不同类别井实体 → 符号各自命中断言。
3. 图例联动：增删图层后图例项随之增减，swatch 与图层渲染一致（可像素抽样）。
4. 全量绿：`tst_layertreepanel`、`tst_ui`、新符号用例；`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（`## Summary`/`#### Test plan`/自审结论清单/遗留项）。**不合并、不推 master、不等 CI。**
