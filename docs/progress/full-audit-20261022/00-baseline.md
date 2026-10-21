# Full Audit 2026-10-22 — 00 基线

## 基本身份

- BASE_SHA：`b301f2e8fbfbcf0dde508d75c2376c8c05d6e4c2`（origin/master，2026-10-22 fetch）
- 工作分支：`codex/full-review-fix-review-fix-20261022`
- Worktree：`/home/kevin/projects/paleo_workstation-full-review-20261022`（独立 build/，不覆盖任何既有 worktree）
- 主 checkout 未提交改动保留（仅 `.zcode/` 与 `docs/progress/review-from-zero-20261019.md` 未跟踪，未触碰）。

## 本轮审查重点面（增量代码）

上一轮全仓整顿收口于 #607（093346e6，round 5）。其后 master 新增 20 个提交
（#603、#608–#626），共 247 文件、+15766/−3621 行：

- #603 io: 统一测井数据读取与智能导入（XML/文本曲线公共契约 + 预览-确认-提交）
- #608 ci: linux/perf disk guard、flaky ledger
- #609 perf/build: IDW 凸包裁剪 prepared GEOS + 顶层 CMakeLists 拆分
- #610 fix: thread lifetimes / well depth normalization / fault layer leak / sliver overflow
- #611 fix: master 回绿（剖面道距口径、catalog/legend 测试对齐）
- #612 fix(preview): 极值显著性阈值 MAD 稳健尺度
- #613 feat: 空工程竞赛层位不显示 + 测区全景在线底图切换
- #614 改进单井测井网格、相道说明与默认曲线组合
- #615 fix(wellcomposite): 隐藏预测相道后拖拽跳动
- #616 feat(#14): 井位可视化页面加载 GIS 底图
- #617 fix: 三维地震旋转 + 二维色标
- #619 岩心照片放大 + 分层沉积相命名
- #620 test: writeOnnxRaster ORT 门控
- #621 fix(ui): 批量改类型中文名
- #622 fix(app): OpenGL Quick backend 统一
- #623 fix(catalog): GeoJSON polygon rings 保留
- #624/#625 feat/fix(maps): OSM/Esri/NASA 底图预设 + 全图统计阻塞修复
- #626 feat(welllog): 默认测井查看模板、滚轮/Ctrl 缩放、岩性道粒度

热点目录：tests(43)、src/ui/wellcomposite(24)、src/io(23)、src/ui/pages(15)、
src/ui/datapreview(14)、src/workflow(13)、src/qgis(9)、src/services(8)、
src/domain(8)、src/ui/dialogs(7)。

历轮深审覆盖记录：`docs/progress/full-audit-20261019/`、`-20261020/`、`-20261021/`
（评审基线 ee8518df 起）及 review-from-zero 报告（1757 源文件分片评审）。
旧基线遗留问题以 Open Issues #556–#597 登记（外部所有，见 01-conflict-matrix）。

## 平台与工具链

- OS：CachyOS Linux 6.18.55-2-cachyos-lts x64
- 编译器：gcc 16.2.1
- Qt：6.12.0（/usr/lib）
- QGIS/GDAL/GEOS/PROJ：vendored superbuild prefix（主仓共享，只读复用）
  - QGIS_PREFIX=/home/kevin/projects/paleo_workstation/vendor/superbuild/prefix
- 构建类型：RelWithDebInfo，Ninja，`--parallel 4`，全局锁 `/tmp/paleo_workstation-build.lock`

## 基线构建与测试

- configure/build 输出：`/tmp/audit22-baseline-build.log`（结果回填于 06-baseline-quality.md）
- 基线 ctest 结果：回填于 06-baseline-quality.md
- 既有失败 vs 新回归的区分在 Phase 2 完成。

## 资源纪律

- 编译：`flock /tmp/paleo_workstation-build.lock cmake --build build --parallel 4`
- 测试：`ctest --test-dir build -j4 --output-on-failure`
- Subagents：≤5 并发；审计阶段只读。
