# 统一数据导入源码审查（2026-10-20）

基线：master `e6837e14`。只读追踪真实调用链，未凭扩展名或界面文案判定能力。

## 已有能力与缺口

| 来源 / 消费点 | 已证实的代码事实 | 最小修复方向 |
| --- | --- | --- |
| `src/io/wellcompositexml.cpp`，`parseComprehensiveWellXmlData` | SpreadsheetML 可解析连续/离散曲线、地层、岩性、文本道、砂层、取芯、标志层和坐标；曲线保存各自的深度和值。连续曲线表名使用 `startsWith(测井曲线)`。 | 复用解析核，把连续曲线转换为公共井曲线文档，不写一个平行业务库。 |
| `src/domain/wellrecords.cpp`，`sniffWellXml` | 表名仅精确匹配 `测井曲线` 等名称，漏掉解析器支持的 `测井曲线-A1井`；还把 WITSML 标为 WellLog，而综合 XML 解析器只认识 SpreadsheetML。 | 内容识别与实际解析能力一致；未知 XML 保留参考资料能力，不宣传假支持。 |
| `src/io/welllogread.{h,cpp}` | 格式枚举、井名、头、曲线、文档分派只有 LAS/DLIS/LIS；XML 最终进入 LAS 解析。 | 在同一个分派门面增加已实现的 XML/深度曲线文本适配。公共契约仍是 LasHeaderInfo / LasDoc / LasCurve。 |
| `src/io/ingestplan.cpp::proposeWellLog`、`src/io/dataimport_wells.cpp::importWellLogFamily`、`src/io/dataimport_file.cpp::attachResolvableLinks` | 改造前内容井名只从 LAS/DLIS/LIS 读取，XML 依赖文件名主名回退；本次已改为统一井名读面。 | 三处走同一个井名读面；目录名和文件名不能自动创造或合并井。无身份时要求用户选择已有井或显式新建。 |
| `src/services/welllogset.cpp::prepare` | 已决 `well_log` 的当前版本全部经 WellLogRead::parseHeader；失败文件被排除并报告告警。已提供跨文件曲线并集、主文件、重名别名和版本/列溯源。 | 修分派后保留既有角色、主文件与别名契约，避免增设 XML 专用井模型。 |
| `src/ui/pages/datalist_tree.cpp` | 已有井→角色资产树，已显示主文件/成员；综合柱状图分支另按文件名判断。没有单曲线叶节点。 | 复用树；若增加曲线选择，用 assetId + wellId + sourceVersionId + column 定位，不把每根曲线变成独立资产。列表刷新不能同步整读所有文件。 |
| `src/ui/dialogs/folderconfirm.cpp` | 文件预览后还要求“下一步”进入分类汇总，隐藏原表后才显示确认按钮；正常导入重复确认，井身份无法直接选择。 | 原表直接确认；摘要/小表/问题原位呈现；只有真正缺失或冲突的身份才增加选择控件。 |

## 业务消费链

- `WellLogSet::wellCurveIndex` → `PetroPhysTaskService` / `src/services/petrophyscomputeservice.cpp::planWellMerge` → LasCache（经 WellLogRead）→ 多文件曲线对齐与计算。补统一读面可复用该链。
- `src/workflow/wellpredictionservice.cpp` 从井的曲线并集取样，组装 ComprehensiveWellData；目录接口统一不代表读取和轴约束已统一。独立复审发现直接组装入口对空单位按米处理，现改为每文件调用一次 `WellLogRead::parse`，新来源必须明确 MD 和可解释单位，不满足时整井不发预测请求并保留原因。
- 剖面、属性建模与交会已取得井曲线并集，但仍有直接 LAS 读取、裸索引对齐或缺轴校验。实际修复见 `data-import-ux-format-design.md`：保留原算法，收敛公共读面并在真实消费入口检查语义。
- XML 原生预览已有 `PreviewDocService::wellCompositeAt` → `XmlPreviewSession` / `WellCompositePanel::loadComprehensiveXml[Async]`；因此“能画柱状图”不等于“能参与统一业务计算”。
- 单因素砂厚/砂地比：`src/workflow/constraintwellfactors.cpp` 使用 `ProjectDataFacade::topsForMapping` + `WellSectionWorkflow::lithologiesFor`，并优先采用维护的井点属性。`ProjectDataFacade::topsFromFile` 目前读文本分层；井段岩性来自解释/岩屑提供者。仅接 XML 曲线不代表 XML 内地层、岩性已经参与这些指标。后续若接此语义，须明确角色、来源与优先级，不静默覆盖维护值。

## 公共契约适配边界

1. 保留 RAW、资产、版本和 `well_log` 链接；曲线目录以列序定位，列 0 是深度索引。
2. 单曲线文件是“深度轴 + 一条数值道”的普通 well_log，不能因只有一条曲线而当作无效，也不需新目录或数据仓库。
3. SpreadsheetML 连续曲线可适配公共文档；多表不同深度网格应明确拒绝或按有据策略对齐，不能按数组位置错配。离散点不能自动插值伪装连续测井。
4. 井名、轴名/基准/单位、曲线名/单位、有效范围、样本问题进入预览 DTO；解析错误在确认表呈现，执行前再按真实源验证。
5. 显式归属由预览一路传到执行会话；新增井必须来自内容身份或用户明确动作，无名文件不能凭文件名自动建井。

## 与现有 PR 的边界

实查 #580、#581，均为 OPEN，不修改其 worktree，也不重复其修复。

- #580：ingestplan 扫描进度递增；WellLogSet 无轨迹时 MD 替 TVD 的告警；folderconfirm 提取 `pickPreviewFocusAsset`；datalist_tree 析参考资料导航。这里新增统一格式能力与身份选择，不重新实施这些修复。
- #581：crossplot 的 SATR 取消谓词、datalist_batch 删除/三级相禁用原因、单因素建议的行归属校验。均不解决 XML 读面或单曲线选择。

合并时注意 folderconfirm / ingestplan / datalist_tree 的同文件重叠；保持两条 PR 的现有语义，规模基线只缩不涨。

收尾再次检查开放 PR #599（预测 readiness/来源与凭据治理）、#600（窄栏 UI）和
#601（算法原子化），均未合并；#599/#601 与本次 `mappingworkbench_predict.cpp`
有同文件重叠。本次只接统一测井读取和轴单位拒绝，不重做 readiness、安全配置、
发布/Processing Adapter。其余开放审计 issues 不纳入本次修复。
