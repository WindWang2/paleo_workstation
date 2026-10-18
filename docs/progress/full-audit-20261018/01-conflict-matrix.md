# 外部工作排除矩阵（full-audit-20261018）

BASE_SHA=366aef97。任何 Finding 命中下列条目即标记 `EXCLUDED_EXISTING_ISSUE` /
`EXCLUDED_ACTIVE_WORK`，只记录不改码。

## 1. 开放 Issues #460–#503（全部，正文快照 99-external-issues-snapshot.txt）

全部 44 个开放 issues 均被 PR #518 声称处理。要点签名（去重用）：

- #460 tileinference 去垫片形状校验前索引张量（堆越界）
- #461 paleoWriteFileAtomic 只 flush 不 fsync
- #462 PaleoProjectStore::commitAll 回调无 try/catch
- #463 seismic3d 相机书签字段数不对称
- #464 busy-token 跨工程残留
- #465 saveEditingVersion 部分完成态 / 重试复制 spatial_edit_events
- #466 LasParser::parse() 旧入口无大小闸无编码嗅探
- #467 DLIS/LIS parseHeader 无上限
- #468 SGS 实现数×格点数内存闸缺失
- #469 远端预测响应体无大小上限
- #470 cluster 子域 std::distribution/shuffle 不可复现
- #471 wavelet 高斯消元主元绝对阈值
- #472 Qgs* 越层散布 112 文件
- #473 attachWorkbench 1060 行巨型函数
- #474 check_layering --symbol-audit/--transitive 无防增长闸
- #475 master 文档化红项（golden 17/48 超容差）
- #476 domain/catalog/metadata P2×5 P3×5
- #477 io P2×5 P3×7
- #478 algorithms P2×5 P3×6
- #479 services/ai/linkage P2×2 P3×6
- #480 workflow P2×4 P3×5
- #481 ui 主壳 P2×5 P3×6
- #482 ui 专业页面 P2×3 P3×4
- #483 qgis/app P2×1 P3×8
- #484 构建/测试/CI P2×4 P3×5
- #485 swarm 16×P1 追踪 + 9 域低危清单（ Umbrella）
- #486 无组标注井默认模型兜底 + GL 看门狗重试
- #487 测井相点图例层位分组
- #488 singlefactor CRS/约束几何/编辑一致性
- #481–#503 …（详见快照）
- #489–#494 opt-01..05 深化 backlog（大范围待办，非即时缺陷）
- #495 clang-tidy unnecessary-copy（mappingworkbench_impact）
- #496 singlefactor 二轮缺口（叠加 #488）
- #497 CI 门禁失效 / #498 linux-asan 磁盘耗尽 / #499 Windows 句柄释放 /
  #500 lint 红 mappingworkbench_impact.cpp:430 / #501 绿灯通胀 / #502 视图层规模 / #503 测试膨胀

## 2. PR #521（codex/facies-topology-editing，映射相面拓扑编辑）

触碰文件：src/qgis/facieshierarchyrenderer.cpp、faciespartitioneditor.*、faciespartitiontool.*、
mappingfeatureidentity.cpp、src/ui/edittools/**（editingtoolbar*、vertexeditortools、vertexpartitionvalidation）、
src/ui/paleomainwindow_attach_shell.cpp、paleomainwindow_workbench.cpp、src/ui/ribbonpanels.cpp、
src/workflow/mappingevidencereview.cpp、mappinghierarchy.cpp、mappingworkbench.cpp、mappingworkbench_edit.cpp、
tests/tst_edittools.cpp、tst_faciespartition.cpp、tst_mappingworkbench.cpp、tst_ui.cpp、tools/*-baseline.txt。
→ 相面分割/覆盖绘制/拓扑合并、edittools、mappingworkbench 编辑路径 = EXCLUDED_ACTIVE_WORK。

## 3. PR #518（goal/issue-sweep-20261017）

触碰文件覆盖 ai/onnx*/remote*/wellfacies、algorithms/paleoalgorithms、catalog/datacatalog*、
io/lasparser、sattrio、services/jsonsidecar、ui/datalist*、paleomainwindow*（attach/workbench 各件）、
seismic3dviewpanel、wellcomposite/*、mappingworkbench_edit、ci.yml、check_layering/check_qgis_spread/
ci_triage_reds、各 baseline.txt（见 00-baseline.md 与 gh pr view 518 --json files）。
→ 这些文件上的修复若与 #460–#503 任一根因同源 = EXCLUDED_EXISTING_ISSUE。

## 4. 主仓 master 未提交导出功能簇（EXCLUDED_ACTIVE_WORK）

src/domain/exportrequest.h、src/qgis/dataexportwriter.*、src/qgis/geologicalmaplayout.*、
src/services/dataexport.*、src/ui/dialogs/dataexportdialog.*、geologicalexportdialog.*、
src/workflow/dataexportworkflow.*、geologicalexport.*、tests/tst_dataexport.cpp、tst_exportpipeline.cpp、
docs/EXPORT_PLAN.md，及 M 状态的 layoutexport/mapbooklayout/horizonbatchexport/mapexport 等
（完整清单见 00-baseline.md）。导出链路 = 不修，只记录。

## 5. 判定规则

1. Finding 与任一 issue 正文（快照 99-external-issues-snapshot.txt）同根因 → EXCLUDED_EXISTING_ISSUE。
2. Finding 文件+主题与 PR #521/#518 或主仓未提交簇重合 → EXCLUDED_ACTIVE_WORK。
3. 可隔离的独立子问题 → 缩小范围修复并注明依赖边界。
4. 无法隔离 → BLOCKED_BY_EXTERNAL_WORK，只记录。
