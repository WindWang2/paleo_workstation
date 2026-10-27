# 基线质量记录（Phase 2 → Phase 7 会话内维护）

## 构建基线

- configure：Ninja + RelWithDebInfo，QGIS_PREFIX=主仓 vendor/superbuild/prefix（只读共享）
- 全量构建：2551 目标 **exit 0**（首建于 projects 分区 2201/2551 处耗尽磁盘——
  938G 分区被 4 个 worktree 构建占满；迁移 build 目录至 / 设备 765G 空闲后
  以共享 ccache 重放完成；基线日志 baseline-build.log）
- 测试环境注记：ctest 需 `LD_LIBRARY_PATH=<prefix>/lib`（传递依赖
  libqgis_native/libdraco 的 RUNPATH 不传递，CI 同款注入）。

## 测试基线（pristine master 0b9c0a7a）

- **tst_taskservice：17 通过 / 2 失败**——`overlappingLayerTasksHoldBusyUntilLastFinishes`、
  `cancelledOverlappingLayerTaskStillCounts`（F-01 的红基线：#815 L-02 修复
  把 markLayerFree 换成 releaseLayerBusy 但保留计数归零闸，同层双任务先终态
  者的 busy 条目泄漏，层门死锁）。
- **tst_mappingworkbench：exit 134（QgsSettingsException）**——主 worktree 的
  pre-#815 构建同样失败 ⇒ EXISTING_BASELINE_FAILURE（环境）：vendor QGIS
  soname=4.2.2 但内部版本 4.2.3，QgsApplication 运行期按版本号 dlopen 到
  系统包 /usr/lib/libqgis_{core,gui,analysis,native}.so.4.2.3，双 QGIS core
  静态设置注册表冲突即 abort。与 #774「5 个测试审查环境确定性失败」同族。

## Fix 1 验证（改动后）

- tst_taskservice 19/19 ✓（红→绿）
- tst_aichatcontroller 18/18 ✓（含新增 emptyIdToolCallsAreNotDeduplicated）
- tst_wellcomposite_framework ✓（含新增 CSV 转义断言）
- tst_wellattributes ✓（含新增同批重复 key 用例）
- tst_constraintstore / tst_previewdoc / tst_cache_pyramid / tst_pyramid_consume ✓
- 回归批：tst_import_ux / tst_importqueue_progress / tst_panels_dataops /
  tst_upscale / tst_faciesmapping_algorithms / tst_aichattoolloop / tst_aichat /
  tst_datapreview / tst_previewmap_tools / tst_singlefactor_kernel /
  tst_singlefactor_parity ✓（唯一失败 tst_mappingworkbench 为上述环境基线失败）
- F-05（mappingartifactwriter）与 F-13/F-14（workflow 家族）的行为验证载体
  tst_mappingworkbench 在本机环境不可用——以代码复核 + 独立 Review 2 兜底。

## 全量 ctest（Phase 7 验收，F-24 修复前口径）：473/483（98%），10 失败逐项归因

| 失败 | 归因 |
|------|------|
| layering_transitive | 文档注明的本地非闸口（需 QGIS_PREFIX，权威闸在 CI lint 车道） |
| file_budget_strict | #815 缩短 wellsectiondialogs 函数未同步下调基线（可收紧 1）；基线文件在主 checkout 在途清单（EXCLUDED_ACTIVE_WORK），本任务不抢修 |
| tst_assetops | **#756** 在册持有（rollbackExternalIsZeroCopy） |
| tst_wellfacies | **#800** 在册持有（预算 4M→16M 回退的测试面：放大夹具不再被拒） |
| tst_startup_trace | perf-host 环境敏感（标签 envdebt_perf-host；基线 0b9c0a7a 同红、负载相关） |
| tst_versions / tst_derivationgraph / tst_impactrecompute | QgsSettingsException 环境崩溃（双 QGIS 载入；主 worktree pre-#815 构建同样崩） |
| tst_welllog_ui | 主 worktree pre-#815 构建同样失败（既有） |
| tst_predict_challenger_r2 | **F-24（本轮修复）**：#815 把 class_index 类号校验收紧到通道数（恒 1），非 0 类号全转无数据 255，class_index 预测全灭——修复后 18/18 |

结论：10 项失败中 2 项在册 Issue 持有、5 项环境/既有、1 项闸口非权威、
1 项被在途工作阻塞（登记）、1 项为本轮 F-24 已修复。**无一项为本任务
改动引入的回归**（关键证据：全部失败在 pristine 0b9c0a7a 基线上复现或
在 pre-#815 构建上同样失败）。

源码闸门（全绿）：check_layering --strict / --qgs-audit --strict（新增 0）、
check_qgis_spread（新增 0）、check_ui_invariants --strict、check_i18n、
check_ui_tokens（violations=0）、git diff --check。AI 域 ctest -L ai 38/38。
