# 方向 44：井曲线多格式读口——DLIS/LIS + MD/TVD 对齐 + 挂接语义收口

状态：已落地（分支 `goal/welllog-fmt-20261004`）。台账：
`.goal-loop-ledger-welllog-fmt.md`（规范逐条对账/白名单证据/BE 调查）。

## 读了什么

- **DLIS (RP66 v1)**：`src/io/dlisparser.*`——SUL/VR/LRS 流式走查 +
  通用 EFLR 组件机器（Set/模板/对象/特征位全语义）+ FDATA 全帧解码。
  井名（ORIGIN.WELL-NAME）/全曲线目录（CHANNEL）/单位/深度基准
  （FRAME.INDEX-TYPE）读全；表示码含 ISINGL(IBM)/VSINGL(VAX)/FSHORT/
  校验对四种非常规浮点。
- **LIS79**：`src/io/lisparser.*`——TIF 磁带映像自动解包（12B 小端标头链，
  经 dlisio 公开夹具逐字节核实）+ 裸 PR 流 + 多 PR 拼接 + 信息记录井名
  （Wellsite WELL 分量）+ DFSR（条目块/Spec Block subtype 0/1）+ 深度模式
  0/1（模式 1 有向帧距步进，UP 递减）+ 缺席值→NaN。四种 LIS 浮点
  （符号-幅值式尾数）实现并互逆断言。
- **分派门面**：`src/io/welllogread.*`——内容嗅探优先（SUL/磁带标头），
  扩展名兜底；产出 LasParser 同构契约（LasHeaderInfo/LasCurve/LasDoc），
  消费面（welllogset/lascache/previewdoc/导入井名提取 ×2/ingestplan）
  全部改走门面，格式无感。分类器词表登记 `.dlis`/`.lis` → well_log。

## 白名单（显式不支持子结构，逐项有测试证据）

加密记录/帧；多维与复数通道（槽位按宽跳过不错位）；NO-FORMAT 记录；
第二逻辑文件（DLIS）/第二 logset（LIS）——单井单深度轴表格契约；
LIS 快道（samples>1）与字符串/掩码道；模式 1 方向未定义（不猜）。
目录冻结在首个有数据的帧/logset（数据流中后到的定义改写告警不生效）。

## MD/TVD 对齐（诚实口径）

- `LasHeaderInfo.indexBasis`（"MD"/"TVD"/"TIME"/""）：LAS 惯例 MD、
  DLIS 按 INDEX-TYPE、LIS 按 TVD 过程位。`WellLogFile.indexBasis/format`
  透出到并集扫描面。
- petrophys 并集合并：TIME 基准副文件经井时深表主链逆插值
  （`TimeDepthTool::interpolateDepthAtTimeMs` 新增，同源契约：文件序/
  严格递增/不外推）换到驱动深度域再重采样。结果逐副文件记
  `WellResult.notes`（进任务明细）：「已按时深表对齐」或
  「线性重采样（…未对齐/无时深表/表不可用）」——绝不冒充已对齐。
  扫描层对基准不一致逐文件记 WellLogWarnings。

## 挂接语义收口

**主文件只由显式操作变更**：attachLink 不夺主（目标已有已决主关联 →
成员；无主才补主，与导入「首条主」一致）；显式夺主唯一入口 =
setLinkPrimary。依据：canonical 别名/驱动文件选举/剖面快照恢复/
mapping logVersion 都读 isPrimary，挂接换主会造成静默漂移（快照恢复
直接拒）。原 applyPendingResolutions/attachResolvableLinks 的「有主不挂」
守卫与新契约归一。测试：tst_catalog（不夺主/补主/显式夺主/往返）、
tst_assetops（提议面+执行面）。

## 失败诚实面

- 解析器：截断/坏段 error 给因 + LasIssue 分级；半帧/重叠帧专项断言
  （tst_dlisparser/tst_lisparser）；截断文件已解部分不冒充完整。
- 导入：井名提取失败 → 链接 note「测井头解析失败：<原因>」→ 台账行
  （tst_import truncatedDlisImportNotesReason）。零静默。
- BE：查无公开规范定位（ledger §4 调查）——不给假能力（.xls 先例），
  走 reference 口径显式呈现；待样件/出处另立补齐。

## 递延

- LIS 快道子帧布局（samples>1）与多维 DLIS 通道的展开消费。
- TVD↔MD 错位的测斜反推对齐（当前如实标「线性重采样（未对齐）」；
  readCurveTvd 已有 MD→TVD 显示面）。
- 综合图/交会等其余并集消费面接入 TD 对齐（当前对齐落在 petrophys
  合并 + WellLogSet 助手面，其余面可按同助手中继）。
- BE 格式（待规范出处/样件）。
