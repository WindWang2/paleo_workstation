# Full Audit 2026-10-22 — 06 基线质量（Phase 2 回填）

## 基线身份

- BASE_SHA：b301f2e8（origin/master 2026-10-22 fetch）
- 平台：CachyOS Linux 6.18.55-2-cachyos-lts · gcc 16.2.1 · Qt 6.12.0 ·
  vendored QGIS 4.2.2 superbuild prefix（共享只读）
- 配置：Ninja · RelWithDebInfo · `--parallel 4` · 全局锁 /tmp/paleo_workstation-build.lock

## 基线构建

- 首跑：链接失败（ENVIRONMENT_FAILURE，见 00-baseline/09-ledger）：共享 prefix 的
  libarrow/libparquet 5 个悬空符号链接（10-20 会话指向已删除的 appimage 暂存）。
  从 pacman 缓存 arrow-25.0.1-7 提取真实库替换后恢复。BUILD_EXIT=0。
- 纯净树（stash 修后）增量重建 BUILD_EXIT=0。

## 基线测试（纯净 b301f2e8，`ctest -j4 -LE 'core|perf'`，带 prefix LD_LIBRARY_PATH）

- **407/410 通过（99%）**，3 红全部在纯净树复现 ⇒ EXISTING_BASELINE_FAILURE：
  1. `tst_onlinebasemap::tileProviderReadsLocalPixels`（像素 fuzzy 对比）——
     该测试文件属开放 PR #627（EXCLUDED_ACTIVE_WORK），不在本轮修复面。
  2. `tst_datapreview::wellLogPreviewShowsCuttingsLithologyTrack`（值对比，
     tst_datapreview.cpp:322/:448）——master 在册红；与本轮改动面
     （requestSummary 消费端）不同用例，终局复跑对照。
  3. `tst_assetpaging::batchTypeOverrideRefreshesViewsAndKeepsFilter`——
     #621 批量改类型中文名的后续面；上轮在册红为 #585（另一用例）。
- 运行口径说明：裸 ctest 缺 LD_LIBRARY_PATH 时 201 项瞬时红（loader 找不到
  libqgis_native）；正式口径带 `LD_LIBRARY_PATH=<superbuild prefix>/lib`
  （与 ./paleo-dev test 的 deb 闭包注入同理）。分类为 ENVIRONMENT_FAILURE
  （首次运行口径错误），修正口径后为上表结果。

## 源码闸门（纯净树）

- E 代理实跑 17 项检查器全绿（layering --strict 零残留、qgs/spread/transitive/
  file_budget/ui_size/ui_tokens/i18n/qgis_versions/docs_drift/flaky selftest/
  ci_known_reds 等）；基线文件 range 内只缩不涨。

## Fix 1 后定向测试

- 七簇 18 套定向套件：三套件首轮红（welllogtext GB 夹具基准用词误用「深度」/
  batchjobqueue 优先级口径暴露 4 参 start 默认 0 矛盾/petrophysbatch 错误通道
  取 res.error 而非井级 error）→ 修正后全绿；其余 15 套件首轮全绿。
- 修后全量 ctest 与闸门复跑见 08-final-independent-review.md 前置的 Phase 7 记录。
