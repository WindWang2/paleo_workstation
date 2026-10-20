# 统一数据导入最终独立定向复审（2026-10-20）

审查基线 master `e6837e14`，工作树 `codex/data-management-import-ux`。
本轮重新读取当前实现与测试源码，针对 review-1、review-2、review-2a
所指出的风险检查真实调用链；仅写本报告，未修改实现、未编译、未运行测试。
本轮静态复审未发现仍需修复的实质阻断问题，可进入作者的最终本地执行验证。
此结论不代替本地编译、测试、源码闸门或实际视觉验收。

## 修复闭环的当前代码证据

| 风险 | 复核的真实实现与结论 |
| --- | --- |
| 预览后换源、读体与复制不一致 | `produceItem` 把预览 expected hash 传给 ImportOptions；`importOneFile` 使用测井源 fresh SHA 比对预览，再复制私有 QTemporaryDir 快照、校验快照 SHA。`inspectLog` 与 `storeManagedRaw` 都读取该快照，最终 RAW SHA 再对比同一 sourceSha。自动建议没有变成用户 override。原先“先验 A，随后完整解析/复制 B”的窗口已关闭；后续源变化不会影响已经验证的快照。 |
| 去重忽略明确归属 | batch skip / 幂等之前以及 importOneFile dedup 都检查明确目标；目标不同拒绝，不凭文件名补挂或隐式建井。 |
| XML 误分类与无名文件改类型 | 流式检查 Worksheet 名保留井斜类型，也能识别晚出现的曲线表；确认框类型改变为 well_log 时动态提供已有井/新井选择。 |
| GUI 目录整读与真实读体绕路 | 受管版本目录元数据用于 WellLogSet / PreviewDocService 的头查询；属性建模、井震剖面、连井剖面、交会、预测准备、直接 WellPredictionService 和预测对比已经追到统一读体。 |
| 新轴混同 MD | 新格式未知/非 MD 基准及未知单位在实际业务消费处有拒绝原因；明确 MD 英尺在需要米的消费面转换。连井警告、交会错误与直接预测 skip 原因有真实出口，不仅停留在 DTO。 |
| R9 综合井图错轴 | WellCurveRef 传递每个来源的 indexBasis / format；实际 DataPreviewTabs 当前来源和兄弟来源都经 `mdAxisInMetres`，明确 MD 英尺转换，TIME/TVD/未知来源不叠加到 MD 轴，并显示文件名、基准/单位和原始数据保留说明。有效兄弟文件继续显示。 |

`tests/tst_import_preview.cpp` 从公开 `openAsset` 入口获取组装后的实际
WellCompositePanel，检查 `currentData()` 中的采样与深度，同时轮流把英尺源作为
当前文档和兄弟文档。它的 3280.839895… ft → 1000 m 断言会抓到原 R9。
不安全轴测试对 CSV / XML 检查实际曲线排除、有效兄弟保留和可见原因，
并核对旧 LAS 无新目录元数据仍能重开显示；不是仅测试转换 helper。

## 按文件跳过与精确取消

按文件选择沿确认表 → WithWells hook → main window → workflow 的
sync / async 路径 → ImportSession.skippedPaths → produceItem 贯通。
跳过发生在源指纹、井目标、dedup、实体创建和 RAW 写入之前；取消勾选后不再
要求目标井，移除对应 forced / targets，估算同步反映本次跳过；旧 hook 没有假装
提供不支持的复选能力。失败行仍可修正类型/目标后单行重试，继续传预览指纹。

取消句柄绑定 weak_ptr<ImportJob>，不按目录或文件名匹配：每个 Job 有独立
atomic token、QPointer task 和 finished 标记。运行中取消既请求任务取消，也在
owner commit 前复核 token；已排队 Job 到队首后直接回取消结果，不启动 worker。
冲突重做仍是同一 Job；已完成句柄对后续同源批次无作用。runImportJob 按值持有
shared_ptr，清空 m_activeImport 后 finish 仍可安全访问。

确认框 rejected / Esc / close / destroyed 在途最多触发一次 cancel hook；
done 后 importing=false，完成关闭不取消。异步 done 先检查 QPointer<QDialog>，
窗口析构后不再回写控件；destroyed 回调只使用值状态和 cancel hook。

对应测试源码覆盖 owner 提交前取消/失败窗口、精确句柄取消首批而同源后批继续、
排队取消不启动 worker、窗口 reject/close/Esc/destroy、销毁后的迟到回包及完成
关闭不取消。它们检查 catalog/任务/窗口实际状态，具备针对原问题的有效断言。

## 恢复与保护

本轮再次核对 `importrecovery.cpp`、`dataimport_session.cpp` 和
`DataCatalog::resolvedVersionPath`：清单先全量验证，只接受工程内 raw / derived
安全受管相对路径；未搬出的 staging 源不冒充本次目标，已登记版本保留，
未登记目标必须摘要吻合才删除。提交拒绝覆盖既有文件；journal 失败清理本次
移动文件，清理失败保留意图与证据。未发现工程外原件被该恢复链删除的实质路径。

现有故障测试源码覆盖目标碰撞、journal 注入失败、全搬未登记、全搬已登记、
摘要篡改和旧工程重开。部分移动、链接/junction、删除失败的运行覆盖仍须按
作者最终测试记录如实说明；不能把本报告的静态核查扩大成所有故障均已运行。

## 执行证据责任与结论边界

作者负责统一执行不超过 -j2 的本地编译、Windows 串行测试及必要源码闸门，
核对失败原因并记录真实结果；本复审没有将尚未运行的测试标为通过。
新 UI 沿用原生控件、主题 token、翻译机制，未发现新的设计系统静态偏离。
高 DPI / 暗色 / 键盘 / 实际截图验收以作者最终证据为准。

本轮先前必修项的代码修复闭环已完成，静态审查放行；若执行验证出现新失败，
须继续定位修复，不能凭本报告直接宣称整个用户目标完成或本地验证通过。
