# 统一数据导入独立 Review 2（2026-10-20）

只读复审 worktree `codex/data-management-import-ux` 相对 master `e6837e14`
的当前实现；仅写本报告，不改实现、不编译、不运行测试。本报告依照
`AGENTS.md`、`DESIGN.md`、本次 plan / audit / format-design / review-1，
追踪真实的入口、读体、提交与消费代码。另一位独立 Reviewer 的定向结果单列在
`data-import-ux-review-2a.md`；本报告不以开发者自评或尚未运行的测试作为通过证据。
作者仍在修复，以下是复审发现与阶段性结论，**不是最终放行**。

## 必须修复

### R9 [P2] 井曲线实际预览仍把新增格式的原始索引全部当作米制 MD

位置：`src/ui/datapreview/datapreviewtabwelllog.cpp:366` 的
`compositeCurve(ref.mnemonic, src.unit, body->at(0).values, ...)`；
`src/ui/datapreview/datapreviewtabs_internal.h:685` 的 `compositeCurve`；
`src/ui/wellcomposite/paneldata.cpp:95` 的 `loadLasCurves`。

复现路径：同一井导入合法 `MD[m]` 文件和 `MD[ft]` 单曲线文件，或者再导入
`TIME[s]` / `TVD[m]` / 基准未声明的曲线文件。打开任何已决 `well_log`
资产，`buildWellLogContent` 通过 `wellCurveIndex` 收集全部兄弟文件，
后台读取之后逐条把原始索引数值交给综合井图。该函数读取了 header，但从未使用
`header.indexBasis` 判断是否适合 MD 井道，也没有按每个来源转换索引单位。
`compositeCurve` 仅转换为 float；`loadLasCurves` 合并这些数值作为同一深度范围，
同时叠加来自分层、岩性、照片的米制 MD 锚点，画布使用深度米标尺。

结果是 3280 ft 被画在 3280 m，TIME / TVD 也会与 MD 曲线错误对齐。
本次新格式导入允许并如实保存这些轴，业务计算入口的新增拒绝检查不能保护该
实际预览入口。仅验证导入后的单位仍在原文件、或 `readCurveTvd` 返回正确，
不足以证明预览正确。

修复方向：由现有服务门面准备适合综合井图的米制 MD 数据；新增格式中的
明确 MD 按单位换算，未知或其他基准应提供原轴表格/独立视图，或清晰拒绝该
叠加并显示来源和原因。不可默默丢弃异常兄弟来源，也不可修改原始文件。
需用实际 `DataPreviewTabs` 路径覆盖 MD[m]+MD[ft] 的同位置叠加，以及
TIME / TVD / 未知基准不会冒充 MD；不必重写图形算法。

## 已观察到的修复与正向证据

- R2：`produceItem` 在 skip / 幂等前校验用户目标；`importOneFile`
  在字节去重时也调用 `validateDuplicateLogTarget`。测井去重不再调用旧的
  文件名补挂，目标不符会失败，不创建额外井。已有测试包含 direct / batch skip
  与已有井 / 新井选项，但本复审未执行这些测试。
- R3：`classifyImportFile` 对较大 SpreadsheetML 流式检查 Worksheet 名，
  保留井斜分类并允许后续真实曲线表升级为 well_log；没有把所有大型工作簿
  无条件抢成测井。
- R4 / R5：owner finished 在 commit 前重新读取任务取消/失败状态；提交意图
  在 produce 面准备，owner 只检查准备结果和移动文件，不同步重哈希全部 RAW。
  `commitPreparationError` 有独立失败路径。
- R6：新导入受管版本保存曲线目录元数据，`WellLogSet::prepare` 与
  `PreviewDocService::lasHeaderAt` 优先使用该目录，正常树刷新与曲线目录
  不必重新完整解析新增 XML/Text。旧版本仍走原先头扫描回退。
- R7：属性建模、井震剖面、预测范围准备与预测对比的读体已切到
  `WellLogRead::parse`，可以追到真实数据体，不只统一了目录枚举；新增格式的
  MD / 单位拒绝或换算已出现在这些调用点。
- R8：修改类型为 well_log 后 `updateAvailability` 会动态创建目标井控件；
  失败行保留修正/单行重试入口，重试同时传目标与预览指纹。
- XML/Text 转换保留 double 原值，按精确索引并集填 NaN，没有新插值算法；
  混井、同表重复索引、逆序、非法数值、列数不符、曲线重名和密集输出预算
  都有明确拒绝分支。

## 恢复与原件保护核查

`recoverOne` 在删除前验证完整清单，只接受 raw / derived 受管相对路径，
解析路径复用 `DataCatalog::resolvedVersionPath` 的分段与 canonical 根约束。
清单对应 stagedSource 仍在时不把目标认作此次移动；已登记版本跳过；未登记目标
须摘要吻合才进入删除清单。未发现已证实的工程外原件删除路径。

`moveStagedFiles` 拒绝覆盖已存在的目标；journal 失败会清理本次已移动文件，
清理失败保留 intent。已登记但 intent 未删的重开路径保留文件。
现有测试源码覆盖全搬但未登记、全搬且已登记、目标碰撞、journal 注入失败、
摘要被替换和旧 LAS 工程，但这些是覆盖意图，不能替代作者的实际测试结果。
部分移动后崩溃、恶意相对路径、链接/junction、删除失败及幂等二次恢复等场景
目前尚无本复审取得的运行证据。

## 边界与待最终核查

1. R1 的入口哈希到再次解析之间的 TOCTOU，以及连井/交会的新轴语义问题，
   由独立定向复审记录在 review-2a；作者正在用稳定快照与业务守卫修复。
   本报告不据最初的 expectedHashes DTO 即宣称它们已完成。
2. 作者在本复审期间追加按文件跳过/保留既有数据交互，尚未冻结，必须在最终
   patch 上复核真实 UI → hooks → workflow → session 的传递与重试。
3. 在途确认框的「取消」目前仅关闭窗口，后台任务继续提交；这与旧实现相同，
   不计本次新增回归。但新交互应使用明确的后台继续文案，或接入已有任务取消，
   避免用户把关闭窗口误解为取消导入。
4. 新 UI 使用 Qt 原生控件、主题 token、monoFont 和翻译机制，静态未发现
   新硬编码品牌色或字体偏离 DESIGN.md。没有截图、高 DPI、暗色及键盘操作
   的实际验收证据，不能由静态阅读推定全部通过。

最终应在 R9、review-2a 必修项以及按文件选择交互修复完成后再次独立核查，
结合作者本地 -j2 编译和真实测试结果给出结论。
