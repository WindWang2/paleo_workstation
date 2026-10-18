# Fix 2 — Review 2 处置结果（full-audit-20261018）

复审输入：06-independent-review-r2.md（五维全量）+ 06-independent-review-r2b-adversarial.md
（对抗性挑战）。两位复审者独立收敛到同一 MUST_FIX。

## MUST_FIX 处置

| ID | 原始问题 | 二次修改 | 测试证据 | 最终状态 |
|---|---|---|---|---|
| R2-MF-01（=R2B-MF-01） | readXlsx 的 cellBudget 声明在 sheetTargets 循环内，每表重置 4M 预算——多表恶意簿（每表 244 行 XFD 引用 ≈4M 格）合计 N×4M 仍可放大驻留，与 .xml 路径全簿口径及「整簿拒收」文案三方分裂 | 预算声明上移到 sheet 循环外（outsourceworkbook.cpp），xlsx 与 .xml/wellcompositexml 三读口同为整簿累计口径 | 新增 tst_io_outsource::rejectsMultiSheetXlsxAmplification（zipWriteArchive 构造 3 表放大簿 → 拒收）；红绿已验（还原缺陷形态该测试红、修复绿） | FIXED |

## SHOULD_FIX 处置（全部采纳修复）

| ID | 原始问题 | 二次修改 | 验证 |
|---|---|---|---|
| R2-SF-01 | FB-001 测试尾断言 contains(3) 对含 3 的输入恒真——并列裁决方向未钉 | 换 golden 面积集：{3: 5.0 格, 7: 4.0 格}（方向翻转变红） | tst_algorithms 绿；面积由 3×3 推演逐格复核 |
| R2-SF-02 | reloadFrames 遇集合外层位 m_index 可越新表界——帧标签撒谎、步进双向锁死；复位只覆盖无 selection 分支 | 集合外/无激活统一复位 m_index=-1 + 标签清空 | tst_evolutionplayer 全绿（既有 reload 用例回归） |
| R2-SF-03 | publish 的 jobCompleted 与 !task 分支缺 disconnectSourceWatches（save 路径有）——legacy 守卫连接悬挂一代 | 两分支补 disconnectSourceWatches（与 save :382/:392 对称） | tst_versions 全绿（含 legacy 发布用例） |
| R2-SF-04 | ask seam 排在 offscreen 早退之后——CI 永不可达、新分支零自动化覆盖 | seam 判定提前到 offscreen 早退之前（注入契约优先；未注入 offscreen 直存语义不变，生产行为不变） | 新增 tst_edittools::attributeTableHostAskSeamDrivesSaveDiscardCancel（Cancel/Discard/Save 三分支 + 未注入直存回归）全绿 |
| R2-SF-05 | QTemporaryDir 未查 isValid()——/tmp 失败时把 XXXXXX 模板字面量路径传给 LibreOffice | isValid() 失败走 finishConversion(-1) 如实失败 | 编译 + 既有 tst_import 转换族绿（失败分支需 /tmp 不可用，环境不可构造，如实记录） |

## 对抗复审观察（OBS）处置

- OBS-01（fillSgs 恒带回调使 sgs3 走串行档、worker 收缩对生产充填链不生效）：如实记录——
  FB-002 修复面是 geostat 公开 API 直调路径（无回调并行档），充填链串行工作区无上界是
  FB-002/FB-003 修复自己披露的选择（串行档=并行化前既有行为）；DOCUMENT_ONLY。
- OBS-02（memory 层回放丢数据）：全库无调用方可达（INPUT 恒为栅格文件层）；DOCUMENT_ONLY。
- OBS-03 = R2-SF-03 已修。
- OBS-04（出表再回表丢显式设置）：既定权衡（重排不清设置的对称面），已在源码注释与测试钉死；DOCUMENT_ONLY。
- OBS-05（缓存 runtime 旁路 sha 校验）：打包机本地缓存信任面，与 vendored cache 同口径；DOCUMENT_ONLY。
- OBS-06（tst_import 扫共享 /tmp、512MB 稀疏夹具）：环境敏感点记录——/tmp 互扰经分析仅
  tst_import 单进程做文档转换（终审更正：ctest 沙箱已注入 TMPDIR，CMakeLists.txt:885，
  互扰面进一步收窄）；稀疏夹具新代码零读（闸在 readAll 前）。

## 验证汇总

- 定向套件：tst_io_outsource 14/14、tst_algorithms 22/22、tst_evolutionplayer、tst_edittools、
  tst_versions、tst_import、tst_previewmap_tools、tst_previewmap_canvas、tst_wellfacies、
  tst_workflows 全绿（ctest -j4）。
- R2-MF-01 红绿双向验证完成。
- clang-tidy 门禁（merge-base 范围）绿；layering --strict 绿；ui_invariants/i18n/ui_tokens 8/8 绿。
