# 统一数据导入独立 Review 1（2026-10-20）

审查者独立只读追踪 `origin/master` 到当前 worktree 的 tracked / untracked
改动；只写本报告，未编译、未修改实现。依据 `AGENTS.md`、`DESIGN.md`、
`docs/PALEO_QGIS_PLAN.md`、本次 audit / plan / format-design，并采用
engineering:code-review 的正确性、并发、错误传播、性能维度。
作者在审查期间并行修复，以下明确区分首次发现与已观察到的修复；本报告不是复审放行。
#580 / #581 已知基线问题不计为本次新增缺陷。

## 必须修复：首次独立审查发现

### R1 [P1] 预览自动归井被固化为显式覆盖，源文件改变后静默错井

最初位置：`src/ui/dialogs/folderconfirm_preview.cpp:100` 附近
`wellTargets.insert(source.path, source.suggestedWellId)`；执行入口
`src/io/dataimport_file.cpp` / `src/io/dataimport_log.cpp::importInspectedLog`。

可触发：已有井 A1、B1。预览路径 x.csv 内容井名 A1，随后同一路径被替换成
B1 的合法数据，再确认导入。执行会重建 plan、重新解析 B1，但传入的
targetWellId 仍是自动写入的 A1，直接把 B1 曲线挂在 A1。执行期 SHA 比较
只能约束该轮复制，不能证明用户看过的内容与本轮内容一致。

修复要求：预览携带内容指纹，执行前拒绝变化；自动建议与用户明确选择分开。
**审查期间已观察到修复代码**：删除自动 suggestedWellId override、加入
sourceSha256 / expectedHashes 贯穿。仍需验证真实 UI→执行、同尺寸内容变化、
冲突重试、单行重试均使用同一指纹。

### R2 [P1] 同字节去重绕过显式目标井，旧补挂逻辑还能凭文件名猜井

最初位置：`src/io/dataimport_file.cpp:266` 附近 dedup 分支调用
`attachResolvableLinks`；该函数 `src/io/dataimport_file.cpp:434` 附近仅从
LAS/DLIS/LIS 读取内部名，并回退 stem，完全不接收 targetWellId。

可触发：文件已在库，预览后点击“仍导入”，选择新井 B 或已有井 B。
命中 SHA 后忽略选择；已决 A 关联保持 A 且返回已有资产，或旧未决 XML
按文件名补挂到别的井。用户明确归属与结果不一致。

修复要求：dedup 分支同样验证目标井，不得从内部名/文件名覆盖明确选择；
不能满足所选归属时明确失败，不伪报成功。
**审查期间已观察到修复代码**：新增 validateDuplicateLogTarget，well_log
去重不再走旧 attachResolvableLinks。需验证旧未决、相似井和改类型去重。

### R3 [P2] 大型独立井斜 XML 被 Workbook 候选提前抢成测井

位置：`src/domain/projectclassifier.cpp:118–121`。

可触发：>=64 KiB 的 SpreadsheetML，首个工作表是受支持的“井斜-A1”，
没有曲线工作表。原分类器 sniffWellXml 会识别 well_deviation；新增
Workbook 根兜底在 sniff 之前直接返回 well_log。统一曲线读取随后报
“未找到受支持的测井曲线工作表”，已有井斜导入能力回归。
大型普通工作簿也被无条件抢成测井。

修复要求：保留已识别 XML 语义的优先级，只有前缀未能识别类型时才考虑
晚出现曲线表的候选；测试 >=64 KiB 井斜文件与晚出现曲线表两种场景。

### R4 [P2] owner 提交前未再次核对批量任务取消

最初位置：`src/workflow/folderimport.cpp:123–151`；
`src/io/ingestplan.cpp::executeIngestPlan` 仅在 progress 返回 false 时设
session.cancelled。

可触发：worker 已完成最后一次 progress，GUI finished 回调还未提交时
收到取消。旧 finished 只在单文件 discardOnCancel 分支核 task flag，
批量 session.cancelled 仍为 false，因此整批提交，违反新增整批取消承诺。

修复要求：owner commit 前核 task cancel 状态并整批丢弃。
**审查期间已观察到修复代码**：finished 统一复核 cancelled / failed。
需用确定性 barrier 或延迟 owner 回调的测试覆盖此窗口。

### R5 [P2] 新恢复意图在 GUI 提交线程同步重哈希整批文件

最初位置：`src/io/dataimport_session.cpp::commitImport` 调
`src/io/importrecovery.cpp:68–87::writeImportIntent`；该函数逐个 hashFile。

可触发：导入大 LAS / 栅格批次，worker 解析复制后返回 GUI，commit 又同步
读取并 SHA 整批 RAW / DERIVED，界面和取消长时间阻塞。此前提交阶段只有
移动文件和 journal，新增整批读取放大了 GUI 停顿。

修复要求：意图准备移入 worker，owner 仅检查已完成意图并提交。
**审查期间已观察到修复代码**：新增 intentPrepared / commitPreparationError，
owner 不再调用 writeImportIntent。需检查所有 produce 入口和冲突重试均准备，
并验证 preparation 失败不可误标成用户取消。

### R6 [P2] 公共 parseHeader 新增整读语义，破坏现有 GUI 头/体两段式

位置：`src/io/welllogread.cpp:143–148` 新格式 parseHeader 调 parseAdditional，
后者完整构造所有采样和多表并集。
真实 GUI 调用点：`src/ui/datapreview/datapreviewtabwelllog.cpp:277`
调用 wellCurveIndex；`src/workflow/wellsectionworkflow.cpp:190` 的
availableMnemonics 也同步枚举井曲线。

可触发：一口井关联多份接近 64 MiB 的 XML/Text，打开该井的 LAS 预览或
剖面曲线下拉。WellLogSet::prepare 在调用线程逐文件 parseHeader，原先
只读取 LAS 头，现在完整解析所有兄弟曲线；“正在后台解析”提示尚未真正
让出事件循环便可卡住。此路径也没有解析体缓存。

修复要求：使用导入已保存的、版本指纹匹配的目录元数据，或把新格式目录
加载放后台；不能只把导入扫描异步就认为消费端已无同步整读。

### R7 [P2] 原业务消费点仍绕过统一读取器，XML/Text 入库后无法使用

位置：`src/workflow/propertymodelworkflow.cpp:1150`，
`src/workflow/sectionworkbench.cpp:412`。

可触发：井有坐标及模型所需顶底面，唯一 GR 为新导入 CSV/XML。
WellLogSet::wellCurveIndex 正常列出 GR；属性建模直接 LasParser::parse，
解析失败后静默 continue，模型看不到该井。井震剖面也直接 LasParser
解析 ref 来源，不能叠加新格式。audit 中“这些消费者复用并集，无需另接”
遗漏了并集目录之后的数据体读取。

修复要求：实际消费者改走公共 WellLogRead / LasCache，并回归完整业务入口。
切换读取器时必须核对轴语义：propertymodel 原实现把任意轴当作米制 MD，
新格式允许 TIME / TVD / TVDSS / 未知基准，不能把这些数值直接喂测斜轨迹。

### R8 [P2] 改类型为测井后没有选井入口，合法无井名曲线无法恢复导入

位置：`src/ui/dialogs/folderconfirm_preview.cpp:92–95,130–140`，
`src/domain/projectclassifier.cpp:163–170`。

可触发输入：`"MD[m]","GR[API]"\n1,2\n2,3\n`。读取器 CSV split
支持带引号表头，分类器正则不支持，预览先成为 tabular；用户把类型改为
well_log，界面只 updateAvailability，初始 needsWellChoice=false 从未创建
选井框。确认不被阻断，执行失败“请输入新井名称”；重试仍无法选择目标井。
其他误分类后人工改类型的无名曲线有同一问题。

修复要求：类型变为 well_log 可触发重新检查 / 提供目标井选择；分类与读取
应尽量复用 sniff 规则，避免引号、BOM、中文“井深”、分隔符支持不一致。

## 建议与覆盖缺口（与必须修复分开）

1. 新格式保留 double 精度、精确索引并集与 NaN 空位，未擅自插值；
   混井、逆序/重复索引、非有限值及密集输出预算都有明确拒绝路径，这是正确方向。
2. 恢复清单先全量验证、限制受管目录、核摘要、跳过已登记版本，优于无条件
   删除；需覆盖移动一半崩溃、journal 已成功但清单未清、摘要不符、被篡改清单、
   symlink / junction、删除失败、部分已清后的幂等重开，以及 prepared 打开路径。
   本次静态审查未发现已证实的跨根删除缺陷；不能代替故障注入验证。
3. 新 UI 使用原生控件、主题 token、monoFont 和 tr，未发现硬编码品牌色/字体
   违背 DESIGN.md。尚无实际截图及高 DPI / 暗色 / 键盘焦点的视觉验收证据。
4. DialogState 的异步 done 通过 QPointer<QDialog> 防关闭后写 UI，连接以 dialog
   为 context；未发现明显 shared_ptr 自循环。应覆盖关闭确认框后 worker 完成、
   切工程、预览多次触发与异步结果乱序；后者部分属于已有文件预览路径边界。
5. `readCurveTvd`、交会及岩性参数合并已有轴/单位假设，新 XML/Text 会扩大触达。
   有单位/基准差异时必须给出业务层明确拒绝/换算证据，不能只证明原文件保留单位。
   `wellpredictionservice` 已对非 MD 检查，不能据此推定所有消费者都有同样保护。
6. 仍需检查 `mappingworkbench_predict.cpp:495`、`wellpredictionreview.cpp:33`
   直接 LasParser 是只读自产 LAS 还是会消费新 RAW；若只读自产 LAS，应写清边界。

## 复审入口

R1 / R2 / R4 / R5 的修复在审查期间已开始出现，尚未给最终通过。
R3 / R6 / R7 / R8 需在最终 patch 上再核对；测试应覆盖可触发场景，而非仅
对 DTO 和解析器分别自证。最终交付说明应准确列出实际打通的业务消费者。
