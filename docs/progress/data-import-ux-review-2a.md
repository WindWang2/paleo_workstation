# 统一数据导入独立复审 2a（2026-10-20）

范围：当前完整 worktree 对照 Review 1 的 R1–R8，新增跟踪真实消费者。
仅静态读取源码和测试，不编译、不执行构建、不修改实现。作者同时修复中；
报告记录本轮所见，不把“已改源码”当作“测试通过”。依父任务安排先结束本轮，
以下剩余项待稳态 patch 定向复审。

## 尚须修复 / 定向复核

### R1b [P1] 预览摘要只在 produceItem 入口校验，执行期间还可越过

位置：`src/io/dataimport_folder.cpp:488` 附近先比 expectedSourceHashes，
随后进入 `src/io/dataimport_file.cpp:166–198` 的
classifyImportFile → logFileDigest → inspectLog；复制阶段只比较执行期
sourceSha，没有与 preview expected 比较。

场景：入口摘要校验通过后源由 A 换成 B。大型说明 XML 的二次分类扫描会扩大
这个窗口；后续 sourceSha、解析和复制都读到 B，则 B 在没有重新预览情况下
被接受。若显式 target=A，仍可把 B 内容挂 A。另解析与复制是两次独立读取，
文件在解析时临时变化再恢复，也会使元数据与受管字节不一致。

要求：expected 贯穿实际执行并与最终受管字节比较；解析稳定的暂存快照，
确保预览摘要、解析内容、最终 RAW 指向同一字节集合。
已提前通知作者，作者确认列为必修。

### R9 [P1] 真实连井剖面仍将 TVD / TVDSS / 未知基准当 MD

位置：`src/workflow/wellsectionworkflow.cpp:377–435`，
`WellSectionWorkflow::loadCurveBodies`。

该入口通过 LasCache 读到了新格式文档，随后只检查深度单位，再把索引值写入
wellsection::Curve.depths；未读 header.indexBasis。新导入 TVD[m]、TVDSS[m]、
DEPTH[m] 都能进入 MD 剖面语义，并进一步用于井轨迹对齐。新增
WellLogSet::readCurveTvd 的严格基准保护没有被这个实际入口调用。

要求：真实 loadCurveBodies 路径对新 XML/Text 核对 MD 与可换算深度单位；
不满足时显式告警并跳过，测试驱动 WellSectionWorkflow::request。
已提前通知作者，作者确认列为必修。

### R10 [P2] WellPredictionService 组装仍允许新格式空单位按米

位置：`src/workflow/wellpredictionservice.cpp:244–254`。

该消费者虽然检查 indexBasis=MD，但 `!depth.unit.isEmpty()` 才拒绝未知单位，
空单位直接乘 1。新 CSV `MD,GR\n1000,40\n1001,50\n` 会保留空单位且基准 MD，
进入这里被默认为米。新增 mappingworkbench_predict 的范围准备守卫只保护该
调用链，不能证明所有 WellPredictionService 直接请求都会经过同一守卫。

要求：核实真实调用边界；新格式空单位不得转成米，应用同一权威单位词表并
在真正组装入口拒绝/告警。已通知作者，尚未看到修复或反证。

## Review 1 修复核对（源码层面）

| 项 | 本轮独立核对结果 |
|---|---|
| R1 | UI 自动建议不再写成 target override，sourceSha256→expectedHashes→session 的批量与重试传递存在；但剩余 R1b 未闭合。 |
| R2 | dedup 增加 validateDuplicateLogTarget，well_log 不再走文件名补挂；不同目标明确失败，未新建井。存在对应 service 与 batch 测试源码。 |
| R3 | 删除仅凭 >=64 KiB Workbook 根抢占；classifyImportFile 对未知/井斜候选流式扫描后置工作表，发现曲线优先，同时保留独立井斜与普通工作簿。测试覆盖三类。 |
| R4 | owner finished 在 commit 前统一核 task cancel / failed，session 整批丢弃；测试通过 taskAdded 先连接 finished，确定性覆盖 worker 完成后窗口。未运行。 |
| R5 | commit 不再 hash 整批；produceFile / executeIngestPlan 准备 intent，owner 仅检查 intentPrepared / commitPreparationError。inline fallback 仍同步是原同步入口语义。 |
| R6 | 新受管版本保存 logCurveNames / logWellName / logIndexBasis；WellLogSet::storedHeader 与 PreviewDocService 优先消费，通常新导入文件不再为 GUI 目录整读。旧元数据缺失或外链仍回退完整读取，属于明确性能边界。 |
| R7 | propertymodel / sectionworkbench / mappingworkbench_predict / wellpredictionreview 已切公共读取并核基准/单位，旧 LAS 保持兼容。实际 WellSection 漏点见 R9，预测空单位见 R10。 |
| R8 | availableWells 对所有预览行填写；类型切 well_log 动态创建选井框，初次确认与重试均检查目标；quoted CSV 的分类也与 WellLogText::sniffText 对齐。 |

## 恢复与 UI 源码核对

- 恢复先验证整份清单，目标经 DataCatalog::resolvedVersionPath 限制路径；
  仅清除未登记、摘要相符的最终文件，遇到仍有 stagedSource 则不删目标，
  避免 rename 未成功时误删原本占位文件；本轮未发现新的已证实跨根删除漏洞。
- 准备 intent 尚未 move 时不设 recoveryPending，取消/销毁会正常删除 staging；
  owner 开始最终移动才保留恢复凭据。恢复测试源码涵盖未登记中断、已登记中断、
  篡改字节保留证据、journal 失败回滚、重试；实际执行结果待本地构建后验证。
- 新井树 summary helper 只展示现有资产节点的曲线摘要和来源版本，不新增每曲线
  资产/节点，没有在树刷新时解析整文件；沿用图标、tr 和原生树，未见 DESIGN.md
  新增硬编码字体/颜色偏差。仍未进行真实视觉验收。
- CrossplotSources / CrossplotSamples::well 仍传裸索引并按数值对齐，不携带轴基准
  和索引单位。混 TIME / MD、ft / m 可能错配；部分是原有 LAS/DLIS 消费边界，
  应明确新格式支持范围，不把“读取成功且保留单位”当作交会换算正确的证据。

## 本轮结论

不予独立复审通过。R1b / R9 已确认需修，R10 需修复或给出调用链反证；
其余项已核对到具体修复源码和有意义测试，但测试尚未由本审查者运行。
