# Loop Ledger（full-audit-20261020）

## Round 1 — 环境与基线（Phase 0-2）

- **Finding IDs**: 无（环境事件）
- **Changed Files**: 无源码（worktree/vendor symlink/build 接线）
- **Verification**: BASE_SHA=ee8518df 确认；PR #580 + 4 issue 排除矩阵（01-conflict-matrix.md）；
  构建环境事件两起：①共享 prefix libgdal.so.39 依赖 libarrow.so.2500 而系统已升 26——
  以 `-Wl,-rpath-link` 指向主仓 build/appimage 内 Arrow 25 闭包（只读复用，不动 prefix，
  BUILDING.md 禁重建纪律遵守）；②-j4 并行链接四个 QGIS/WebEngine 大目标时 ld 段错误
  （资源事件）——降 -j2 续建。
- **Review Result**: 待独立复审
- **Remaining Work**: 基线全量 ctest；Fix 1 十簇
- **Next Action**: 基线构建完成后跑全量 ctest → Fix 1

## Round 2 — Fix 1 十簇（Phase 3）

- **Finding IDs**: A-10, D-05, D-07, D-10, C-10, A-14/B-03, B-04, A-15/B-05, D-13/D-01残留,
  D-12/D-02, D-11, A-13/A-04, A-11, E-10/E-02, E-12/E-05, E-13, E-11(部分), C-11, C-14, C-15,
  B-11, B-10(最小侵入)
- **Changed Files**: 25 源文件 + 9 测试文件 + CMakeLists + known-reds + ci_triage_reds.py
  + 新增 src/domain/sidecarvocab.h（详见 04-finding-register 状态列与 git log）
- **Verification**: 定向测试与红/绿证据——构建完成后逐簇运行（见 06-baseline-quality.md）
- **Review Result**: 待 Phase 4
- **Remaining Work**: 编译验证 + 定向测试 + commit 分簇 + 全量回归
- **Next Action**: build4 完成后定向测试

### 逐簇记录（补）

| 簇 | Findings | 回归测试 | 结果 |
|---|---|---|---|
| F1 | A-10 (+stored 等长) | tst_io_ziparchive_unit ×2 新例 | 待跑 |
| F2 | D-05/D-07/D-10 | tst_seismic_3dui 扩展（stalePanel 段） | 待跑 |
| F3 | C-10 | tst_paleomaplibrary 新例 + declareMaps 断言 | 待跑 |
| F4 | A-14/B-03 | tst_storagegovernance 新例 + tst_paleomaplibrary members 新例 | 待跑 |
| F5 | B-04 + A-15/B-05 | tst_sattrio 新例 + tst_crossplot_samples 新例 | 待跑 |
| F6 | D-13 + D-12 + D-11 | tst_wellattributes 新例 + tst_mappingworkbench 扩展 + tst_paleomaplibrary 扩展 | 待跑 |
| F7 | A-13 + A-11 | tst_catalogstore 新例 + tst_segy 新例 | 待跑 |
| F8 | E-10/E-12/E-13/E-11部分 | ci_triage_reds selftest 17 夹具（已过）+ ctest 注册面对账 | selftest 已过 |
| F9 | C-11/C-14/C-15 | tst_paleomaplibrary 新例（C-14/C-15 静态修复 review 钉） | 待跑 |
| F10 | B-11 + B-10 | tst_singlefactor_contract 补例（B-11）+ tst_faultsurface 新例（B-10） | 待跑（B-11 测试待补） |
