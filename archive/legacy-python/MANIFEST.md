# MANIFEST — archive/legacy-python 归档清单

- 归档基线：`39515ad9`（origin/master @ 2026-10-25）
- 归档分支：`goal/archive-legacy-python-20261025`
- 迁移方式：`git mv`（内容零改动，Git 历史随 `--follow` 可追溯）
- 归档时间：2026-10-25

| 原路径 | 新路径 | 大小（字节） | SHA256 | 入库历史 | 原用途 | 当前状态与退役证据 | C++ 替代 |
|--------|--------|------------|--------|---------|--------|-------------------|----------|
| `artifacts/gen_batch_status.py` | `original/artifacts/gen_batch_status.py` | 6580 | `a4789b377e295872ec7cffda1b1ec7fbc389d190bbdf66222a348d21a218d444` | e6837e14（#598，2026-10-20）唯一一次入库 | 生成 2026-10-16 issue #497–#504 批次交付状态一页 docx 报告（python-docx） | 一次性评审产物生成器：全仓零引用（CI/CMake/ctest/脚本/文档均无）；输出路径硬编码 `C:\Users\wangj.KEVIN\projects\paleo_workstation\artifacts\issue-批次交付-2026-10-16.docx`；产物 docx 已随同一 commit #598 入库（自包含交付物在档，生成器再无运行需求）；#598 提交说明自述为「收尾提交——工区评审产物…（docx/批量状态脚本）」工作区遗留 | 无需替代（评审交付物已定稿在库，非产品能力；同类后续报告按需重写即可） |
| `artifacts/gen_review_doc.py` | `original/artifacts/gen_review_doc.py` | 9089 | `0c5007be6ba82a61b8b5cf87a722ef67b939f671242e639371a94563f92d33c5` | e6837e14（#598，2026-10-20）唯一一次入库 | 生成 2026-10-16 paleo_workstation 整体性评价一页 docx 报告（python-docx） | 同上：零引用、硬编码他人机器绝对路径、产物 docx 已随 #598 入库、#598 收尾提交定性为评审产物遗留 | 无需替代（同上） |

## 迁移时引用对账

两文件在全仓（源码、CMake、CI workflow、ctest 注册、shell/PowerShell、
文档、打包路径）均无任何引用，迁移不产生悬空引用，无需修复调用方。

## 未归档说明（防回升）

以下易混淆文件经审计确认**活跃**，禁止归档（完整证据见
`docs/progress/python-legacy-audit-20261025.md`）：

- `tools/`、`scripts/`、`tests/`、`testdata/`、`tools/reference/` 下全部
  在册 Python 工具/夹具（B/C/D 类）；
- `spikes/onnx/`（被 `tst_onnxworkflow` 作为夹具源使用）；
- `vendor/` 供应链脚本（E 类）。
