# Full Audit 2026-10-17 — 01 外部工作排除矩阵

规则：与下列任何条目根因重合的 Finding 一律标记 `EXCLUDED_EXISTING_ISSUE` /
`EXCLUDED_ACTIVE_WORK`，只记录不修复；无法隔离的依赖标 `BLOCKED_BY_EXTERNAL_WORK`。

## A. 既有 Open Issues（33，全部外部持有）

| Issue | 主题（缩写） |
|---|---|
| #460 | tileinference 去垫片形状校验前索引输出张量 |
| #461 | paleoWriteFileAtomic 只 flush 不 fsync |
| #462 | PaleoProjectStore::commitAll 回调无 try/catch |
| #463 | seismic3d 相机书签字段数不对称 |
| #464 | busy-token 跨工程残留 |
| #465 | saveEditingVersion 部分完成态 + 重试复制事件 |
| #466 | LasParser::parse() 旧入口无大小闸 |
| #467 | DLIS/LIS parseHeader 头扫描无上限 |
| #468 | SGS 实现数×格点数内存闸缺失 |
| #469 | 远端预测响应体无大小上限 |
| #470 | cluster std::distribution/shuffle 不可复现 |
| #471 | wavelet 高斯消元主元绝对阈值 |
| #472 | Qgs* 越层散布 112 文件 |
| #473 | attachWorkbench 1060 行巨型函数 |
| #474 | check_layering --symbol-audit 无闸 |
| #475 | master 文档化红项（golden 17/48 超容差） |
| #476 | domain/catalog/metadata 低危清单 P2×5 P3×5 |
| #477 | io 低危清单 P2×5 P3×7 |
| #478 | algorithms 低危清单 P2×5 P3×6 |
| #479 | services/ai/linkage 低危清单 P2×2 P3×6 |
| #480 | workflow 低危清单 P2×4 P3×5 |
| #481 | ui 主壳低危清单 P2×5 P3×6 |
| #482 | ui 专业页低危清单 P2×3 P3×4 |
| #483 | qgis/app 低危清单 P2×1 P3×8 |
| #484 | 构建/测试/CI 低危清单 P2×4 P3×5 |
| #485 | swarm review 16×P1 追踪（= #460-#474 之源） |
| #497 | CI 门禁功能失效 |
| #498 | linux-asan 磁盘耗尽 |
| #499 | Windows 句柄未释放（工作副本/GPKG 删除失败） |
| #501 | 绿灯通胀（环境债豁免/flaky 放行） |
| #502 | 视图层规模失衡 |
| #503 | 测试侧膨胀（44min ctest） |
| #504 | 工作区/仓库卫生 |

## B. 活跃 PR / 分支

| 引用 | 范围 | 处置 |
|---|---|---|
| PR #506 (codex/ci-green-20261016) | CI workflow / 测试 / tools（#497–#504 批次） | EXCLUDED_ACTIVE_WORK |
| codex/audit-remediation 769b8e3d（已推送） | 35 文件 P0/P1 修复：officepreviewrenderer、lasparser、lisparser、velocitymodel、inversion/volume、wavelet、tileinference、remotepredictrouter、faultsurface、constraintparse、localidw、atomicfile、paleoprojectstore（try/catch + token 清理）、datacatalog、seismictaskservice_attr、inversionworkflow、faciesclassificationservice、mappingworkbench(_edit)、mappingworkbenchpage、datapreviewtabs_well、seismic3dviewpanel、exportengine、mappingworkflow、mapversioncontroller_async、CMake UBSAN、CI layering 检查 | EXCLUDED_ACTIVE_WORK（根因重合者排除；可复核其正确性但不重复修改） |
| codex/audit-remediation 未提交改动（主 checkout） | src/qgis/mappingartifactwriter.*、previewmapcanvas、qgislayerservice、ui/datapreview/datapreviewtabgeojson、previewmappage、ui/pages/constraintpage*（7 文件）、paleomainwindow_workbench、workflow/mappingworkbench、tst_mappingpages/tst_mappingworkbench/tst_previewmap_page、tools/singlefactor_run.cpp | EXCLUDED_ACTIVE_WORK（硬排除：正在被另一会话写入） |
| codex/fix-open-issues-20261016 | #478 测试补齐、#463 相机书签 round-trip 等既有 issue | EXCLUDED_ACTIVE_WORK |
| codex/well-attribute-tables | mapping tops set 读取（20kou → tops set） | EXCLUDED_ACTIVE_WORK |
| codex/mapping-evidence-deepening | = master（#494 已合并），无未合并改动 | 无冲突 |

## C. 已合并但值得复核（不排除，属本轮主攻）

- #496 (a1efbd74) singlefactor 二轮收口 —— 未经任何审计
- #495 (5103cb3f) clang-tidy copy-init —— 未经审计
- #489–#494 opt-01..05 + mapping evidence —— 仅被 audit-remediation 静态覆盖，其修复未独立复审

## D. 主 checkout 未提交改动保护

主 worktree（/home/kevin/projects/paleo_workstation）的一切内容**只读**；
本轮所有写入只发生在 /home/kevin/projects/paleo_workstation-full-review-20261017。
