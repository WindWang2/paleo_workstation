## 问题

**测试侧膨胀 + 反馈周期过长**：18.4 万行测试码、巨型测试单文件、Windows 全量 ctest 约 44 分钟。

## 证据（master @ ddd1325e / run 38835970978）

- 体量：`tests/` 389 文件 **184,033 行**；最大单文件 `tst_panels.cpp` 5,337、`tst_edittools.cpp` 4,426、`tst_import.cpp` 3,485、`tst_mappingworkbench.cpp` 3,184、`tst_correlation_full.cpp` 2,565。
- 墙钟：windows job `Total Test time (real) = 2635.06 sec`（≈44 min）；label 耗时 ui 845 s*proc（90 tests）、workflow 922 s*proc（58）、qgis 174 s*proc（20）、io 124 s*proc（33）。
- 跨平台差异显著：linux 同为 373 项仅 `147.62 sec`，Windows 慢约 **18×**（MSVC 链接/运行 + QGIS 装载/文件系统）。
- 目标面：注册 ctest 406 项（win 388 / linux 373），CMake 侧 397 处 `add_paleo_test`。

## 影响

反馈周期过长会**反向激励「不跑全量就合并」**，与本批 A（CI 门禁失效）叠加后进一步削弱验证有效性；巨型测试文件则定位失败困难、易互相污染。

## 建议

- 按 label 分片并行车道（ui / workflow / io+qgis / 其余），把 Windows 墙钟压到 15 分钟内。
- 拆分 `tst_panels`（5,337）、`tst_edittools`（4,426）等巨型测试文件，按被测域切成多个 `add_paleo_test` 目标（瘦 relink 面已有机制）。
- 定位 Windows 相对 Linux 的 18× 差距来源（装载 / 链接 / 文件系统 / 沙箱），优先削掉系统性部分，这是性价比最高的一项。

## 验证

Windows 全量 ctest 墙钟 < 15 min，且最大测试文件 < 3,000 行。

## 关联

父项：本批 A；相关：#484（CMake/测试布线清单）
