# 基线质量（full-audit-20261021）

## 构建

- 配置：`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DQGIS_PREFIX=<main-worktree>/vendor/superbuild/prefix`（vendor 只读共享）
- 平台：linux x64（cachyos-lts 6.18.55-2），Qt 6.12.0 / QGIS 4.2.2（vendor prefix）
  / GDAL vendor；ccache 25G 上限
- 磁盘事故与恢复（如实记录）：projects 分区（nvme0n1p1，938G）满，
  完整构建需 ~95G；经历 build 目录迁移失败（相对 include 断裂，BUILDING.md
  已知接线）→ 原路径重建（ccache 同 cwd 全命中）→ 链接期空间不足 →
  对已链 tst 二进制 objcopy --strip-debug 回收（副作用：RUNPATH 丢失 →
  patchelf 恢复）。最终 build 完成度：394/396 C++ 测试二进制
  （tst_dataops_incremental / tst_faciessupervised 两目标因空间未链，
  记 Not Run）；paleo 主程序被删（可重建，不影响 ctest）。
- **测试运行口径**：vendor libqgis_core.so 无自身 RUNPATH 而系统装的是
  4.2.3（soname 不同），qgis 链接的测试需
  `LD_LIBRARY_PATH=<prefix>/lib:<worktree>/vendor/onnxruntime/lib` 运行
  （与上轮 20261019/20261020 worktree 本地实测口径一致——其二进制裸跑
  同样 not found）。

## 基线测试（ctest3，二进制=BASE 构建；脚本闸=含本轮修复的工作树）

- 命令：`ctest --test-dir build -LE 'core|perf' -j4 --output-on-failure`
  （LD_LIBRARY_PATH 如上）
- 结果（回填）：见 07-fix-r1-results.md 终局对账；已知 master 红先行登记：
  layering_qgs_audit（identifytool +2，#604 在修）、layering_transitive
  （ui_qtwidgets_notes 7>6）、ui_size 族（datalist_tree 1100>1094）、
  file_budget 族（E.2-E.5）、ui_tokens 族、docs_drift（27 TU）——
  全部为 review-from-zero E 系/H6 已知在册红（issue #586/#591 族）。

## 代表性性能基准

- A-01 相关：ShaCache 写放大（修复前每 miss 全表落盘）——见修复验证。
- B-1 相关：Ricker 频率形状——回归测试钉死。
- 本轮无其他全局性能重构，无需全量 perf 基线。
