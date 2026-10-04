# Goal-Loop 方向 30：工区数据管线与健康管理——导入向导、体检与回收站

## 背景（实测事实，勿再勘察）

catalog（实体/资产/链接/版本/外链 SHA）已成熟，且**批量导入骨架已存在**：
`src/workflow/folderimport.h`（FolderImportWorkflow：文件夹递归扫描→分类→
确认表预览→行级重试，produce-then-commit 原子提交），回收站亦已有
`DataListPanel::showRecycleBin` 对话框 + `dataopsmodel.h` 的 `RecycleBin`
sidecar（remove/restore/save）。仍薄的是：导入确认表的实体归位预览、
导入历史/报告、健康度聚合面板、回收站批量/物理清理、版本对比面。
现状锚点：`src/io/dataimportservice.h`（DataImportService）、
`src/services/previewdoc.h`（PreviewDocService：`absolutePathForVersion`/
`verifyExternalSha`）、`DataCatalog::linksForAsset`/`indexHealthy`
（`roleDiagnosis` 诊断写链接 note 的机制已存在）、`metadata/` 持久层
（sqlite 已迁）、`recycle_bin.json` 软删语义。
**一切改动经 catalog API 走版本，禁旁路写文件/禁直改索引缓存。**

## 环境接线

```bash
cd /home/kevin/projects/paleo_workstation && git fetch origin
git worktree add .worktrees/data-pipeline -b goal/data-pipeline-20261004 origin/master
cd .worktrees/data-pipeline
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

1. **批量导入残差收编**（`FolderImportWorkflow` 骨架已含扫描/分类/确认表/行级重试——先对照其能力列残差清单再动手）：确认表实体归位预览列（行→目标实体/新井标记预显）、导入完成报告持久化（批次台账可查）、未决链接批量归位入口。
2. **CRS/坐标管理**：工程坐标系显式声明；导入时坐标域校验（非米制/经纬度报警与转换提示）；与 `arearules` 工区米制契约对齐。
3. **格式扩展**：XYZ 网格点/Excel 表格导入路径（GeoTIFF 已走 `src/io/rasterpyramid.h` 金字塔管线，勿重复造；先勘察 `src/io` 复用面）；新格式入 manifest 类型词表前先登记。
4. **健康度仪表盘**：资产体检面板——缺失文件/SHA 失配/未决链接/孤立实体/无版本资产/回收站积压，分类计数 + 可定位修复入口（双击跳转对应页）；数据源复用 `indexHealthy`/`roleDiagnosis`/`verifyExternalSha`，不重造检测。
5. **回收站收编**（`RecycleBin`+`showRecycleBin` 对话框已存在，先审计其操作面列残差）：批量恢复/物理删除（二次确认）/占用统计/过期策略；与 `save()` 幂等语义一致，批量操作走单事务。
6. **版本对比**：同资产两版本文件级/元数据级差异查看 + 回滚入口（回滚=新版本指向旧内容，不删历史）。

## 通用纪律（方向内全程有效）

- **分层**：`src/` 新文件头三行 `// 层：<词表>`；导入/体检逻辑归数据或功能层，视图只发信号。
- **诚实面**：识别不出/校验失败如实列原因，**不猜类型不静默跳过**。
- **资源**：构建/测试一律 `-j8`。
- **vendor**：改 vendor 件登记 `PATCHES.md`。
- **UI**：对照 `DESIGN.md`；i18n 过两门。
- **性能断言**：禁绝对毫秒墙钟；批量导入大目录用抽样式计时+比率门。
- **ledger**：`.goal-loop-ledger-data-pipeline.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审（分层/旁路写文件/版本完整性/事务原子性/错误路径/i18n 六维）→ 修复 → 再 review，**至少两轮零 High/Medium**；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 批量导入：含好坏混合文件的夹具目录 → 好文件全入库、坏文件逐条原因列报、零静默。
2. 体检召回：人工植入缺失文件/SHA 失配/未决链接各一，仪表盘全检出且可跳转。
3. 回收站：删除→恢复 round-trip 资产字段一致；物理删除后磁盘+索引双清。
4. 版本回滚：回滚产生新版本（历史可溯），文件内容与目标版本 SHA 一致。
5. 全量绿：新用例 + `tst_constraintstore`、`tst_datapreview`、`tst_staleness`、`tst_ui_blocking`（回收站相关既有断言）；`layering`×3、`ui_invariants`×3、`i18n`×2、`git diff --check`。

## 收尾

自审干净后 push + `gh pr create`（`## Summary`/`#### Test plan`/自审结论清单/遗留项）。**不合并、不推 master、不等 CI。**
