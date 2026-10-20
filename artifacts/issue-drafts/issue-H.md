## 问题

工作区与仓库卫生：23 个未跟踪文件长期滞留、`.gitignore` 无 `*.log` 规则、5 份 goal-loop ledger 未入库。

## 证据（`git status --porcelain`，master @ ddd1325e，23 项）

- 日志与启动产物：`aqtinstall.log`、`launch.log`、`selfcheck.log`（空）、`launch.bat`、`launch.ps1`、`paleo_mods.txt`、`assistant_mods.txt`
- 一次性调试脚本：`tools/check_crypto_path.ps1`、`tools/debug_paleo.ps1`、`tools/disasm_gdi.ps1`、`tools/test_diag.ps1`、`tools/test_qgis.ps1`、`tools/compare_mods.py`、`tools/probe_tests.py`
- 二进制与大件：`vendor/osgeo4w-setup.exe`、`artifacts/portable/`
- `.gitignore` 中无 `*.log` 相关规则
- 根目录 92 份 `.goal-loop-ledger-*.md`，其中 **5 份未入库**（catalog-split / constraintpage-split / mainwindow-split2 / mkproject-spread / sanitizer-hardening）
- `build/` 体积 85 GB

## 影响

- 一次性排查脚本与日志持续污染工作区，`git status` 信噪比下降，误提交风险上升。
- 未入库 ledger 意味着对应方向的交付证据有丢失风险（TODOS.md 已记「账本入库映射无法可靠脚本化」）。
- #484 末条已记「仓库根**追踪**了取证快照垃圾」（goal-loop-prompts.zip、leak-test-root、r0/r1-red-set.txt），本条是其**未跟踪面**补充，两者互补不重复。

## 建议

- `.gitignore` 补：`*.log`、`launch.*`、`*_mods.txt`、`vendor/*.exe`、`artifacts/portable/`。
- 调试脚本归入 `tools/debug/` 并统一忽略；对有长期价值的（`probe_tests.py`、`compare_mods.py`）显式入库并在文档点名。
- 5 份未入库 ledger 归位入库（可顺手在 `goal-loop-prompts/README` 补「账本」列，回应 TODOS.md 的映射缺口）。
- `build/` 定期回收：删 `.pdb` 等安全可逆项，按批执行 + 每批校验。

## 验证

`git status --porcelain` 归零（除有意保留项），且 92 份 ledger 全部入库或明确标注为在飞。

## 关联

相关：#484（已追踪面）
