# Goal-Loop 方向 95：mkproject 夹具扩散——tst_import/tst_aiwiring 等高价值测试改吃 mini 夹具

## 背景（实测事实，勿再勘察；行号为 2026-10-09 master `32851a1e` 口径）

方向 78（#305）交付了 mkproject 夹具工厂（manifest 参数化 +
mini 数据集 19 文件 155KiB + `tests/fixtures/mkprojectfixture.{h,cpp}`
+ `tst_mkprojectfixture` 五槽自消费），四轮盘点确认其价值
尚未扩散：

- **消费计数仍 3 文件**（fixture 两件 + tst_mkprojectfixture）
  ——仓内仍自建临时工程的测试未迁移。
- **最高价值迁移目标**（四轮盘点建议）：
  1. `tst_import` 导入全链段——与 fixture 的导入断言面重叠
     最高（真实 dataimportservice 生产路径 vs synthetic）；
  2. `tst_aiwiring` 换工程重绑段——AI 工具上下文绑定测试
     需要真实工程夹具（catalog/derivation 注入），现自建；
  3. `tst_io_workbook_edges`——工作簿解析有 mini 可对拍；
  4. `tst_mappingworkbench`/`tst_wellattributes`——临时工程
     构建已在文档命令里，可换 fixture 提真实度。
- **约束（账本口径）**：fixture 测试 RUN_SERIAL + 墙钟 6-8.5s
  ——不宜到处塞；迁移目标是「高价值段」不是全量替换。

## 环境接线（Windows 本机实测口径；方向 72 后统一链）

```powershell
cd C:\Users\wangj.KEVIN\projects\paleo_workstation
git fetch origin
git worktree add .worktrees\mkproject-spread -b goal/mkproject-spread-20261010 origin/master
cd .worktrees\mkproject-spread
./paleo-dev.ps1 build
./paleo-dev.ps1 test
```

等价手工接线见 BUILDING.md:140-152。上面不要先
`cd .worktrees\mkproject-fixtures`：若该旧目录还在，第一次
`cd` 会成功，随后相对的 `cd .worktrees\mkproject-spread`
会进到旧 worktree 里面，而不是本方向的新 worktree。

## 目标形态（建议按序）

1. **R0 迁移面评估**：逐个候选测试盘点——现自建工程构建
   的代码段/断言面/迁移收益（覆盖增量 vs 墙钟成本）；迁移
   清单（预计 3-5 个测试的指定段）记 ledger。
2. **tst_import 导入全链段**：改吃 fixture（工程级 catalog
   断言对拍——synthetic vs mini 生产路径的断言差异如实）；
   保留原 synthetic 断言（双轨：fixture 是增广不是替换）。
3. **tst_aiwiring 重绑段**：fixture 工程 → bindChatToolRunner
   重绑 → 工具上下文快照断言（换工程 A→B 的重绑语义——
   方向 77 交付面的真实路径验证）。
4. **tst_io_workbook_edges 对拍**：mini 的 SpreadsheetML/
   岩屑 CSV 作为解析对拍样本（解析器输出与夹具生成源一致）。
5. **视余量**：mappingworkbench/wellattributes 迁移（低优先，
   ledger 里如实记录未做项）。
6. **测试纪律**：迁移段墙钟对比（前/后）入 ledger；RUN_SERIAL
   标注遵守；失败面不掩盖（fixture 测试红了就是红了）。

## 通用纪律（方向内全程有效）

- **分层**：零 src/ 业务逻辑改动（发现生产 bug 记档移交）。
  **必须允许 CMake 改动。** 现状：只有
  `tst_mkprojectfixture` 编译
  `tests/fixtures/mkprojectfixture.cpp`，并拿到
  `MKPROJECT_BIN="$<TARGET_FILE:paleo_mkproject>"`、
  `MKPROJECT_MINI_DIR`、以及对 `paleo_mkproject` 的
  `add_dependencies`（`CMakeLists.txt` 手动注册，约
  1211–1227 行，刻意不走 `add_paleo_test`）。
  `mkprojectfixture.cpp` 在缺这两个宏时 `#error`。
  `add_paleo_test` 只编译 `tests/${name}.cpp`，不注入这些
  定义。因此「只改 tests/ 与 fixtures/」无法让
  `tst_import` 等吃到夹具。优先做成可复用的夹具目标接线
  （一个 CMake 函数：附上 fixture 源、两个编译定义、
  `paleo_mkproject` 依赖），不要每个测试复制一遍手动注册。
- **行为红线**：既有断言语义保留（fixture 是增广）——每个
  迁移段「改前改后断言集合对照」入 ledger。
- **资源**：`./paleo-dev.ps1` 系；ctest 串行；墙钟约束
  （单测试 ≤15s 红线——超了就拆段）。
- **无人值守**：迁移清单取舍自行定案记 ledger。
- **ledger**：`.goal-loop-ledger-mkproject-spread.md`。
- **多轮 review（硬要求）**：每批 → 测试全绿 → diff 自审
  （断言保留/墙钟/沙箱兼容/无 src 业务逻辑触碰/CMake
  夹具接线可复用/文档 五维）→ 修复 →
  再 review，至少两轮零 High/Medium；Low 记 PR body。
- **提交**：原子提交，中文 conventional 前缀。

## Oracle 验收（逐条需验证证据）

1. 迁移执行：≥3 个测试的指定段改吃 mini 夹具（diff 证据）；
   迁移清单与 ledger 一致。
2. 覆盖增量：生产路径触达清单（哪些解析器/服务从 synthetic
   变为双轨——文件对照表）。
3. 墙钟：迁移段前/后耗时对比入 ledger（fixture 段 ≤15s）；
   RUN_SERIAL 标注合规。
4. 断言保留：每个迁移段改前/改后断言集合对照（零删除——
   只增广）。
5. 全量 ctest 对照 R0 红集合 diff 为空；layering 三档绿。
