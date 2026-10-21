# 统一测井导入：最小实施边界（2026-10-20）

基线 `origin/master=e6837e14`；主 checkout 只读。独立分支
`codex/data-management-import-ux`。已读取开放 PR #580/#581 的文件、说明和
reviews（均无 review）；开放 issues #582–#597 等为其他审计池，不扩大修复。

## 已验证的链路与缺口

`PaleoMainWindow::runFilesImportAt` → `FolderImportWorkflow::previewFilesAsync`
→ `DataImportService::producePreviewForFiles` → `buildIngestPlanForFiles`
→ 确认表 → `produceFiles` → staging catalog + RAW → `commitImport`
→ imported 信号 → DataList/业务快照刷新。

`WellLogSet::wellCurveIndex` 通过 `WellLogRead` 构建多文件并集，已有来源版本、
同名曲线别名与主关联；测井/预测/交会复用此接口。无需平行数据库或新实体树。
但 `WellLogRead` 仅支持 LAS/DLIS/LIS，XML 即使登记为 well_log 也不能进入并集。
`sniffWellXml` 对带井名的测井工作表用全等匹配，和现有综合 XML 解析器不一致。
CSV 仅通用表格；测井导入只读头部取名，坏数据体仍能登记成功；内容名不匹配时
回退文件名可能误挂。确认表有额外下一步，却没有样本/曲线摘要和导入前选井。

## 最小方案

1. 扩展已有 WellLogRead 契约，严格读取已实现的 SpreadsheetML 和明确索引的
   文本曲线；复用工作簿字符串读取核，原始文件按现有 RAW 管理。
2. 不新增持久化 schema。数据仍是 well + asset/version + well_log link；
   新字段只在预览 DTO/导入 session。内容井名唯一匹配可自动绑定；文件名仅提示。
   确认前可选已有井或显式新井；同名曲线按原多文件别名保留，绝不覆盖。
3. 解析完整曲线后才创建资产/井；使用现有 staging/journal 提交，覆盖取消、
   提交失败及重试测试，核查现有文件系统与数据库恢复边界。
4. 复用现有确认框和井树，直接预览→确认；摘要/小表足够日常核对，异常原位显示。
5. 本地编译最多 -j2、Windows 串行测试；独立审查后修复复审；PR 不等待远程 CI。

## 兼容性与明确不做

保留旧文件、版本和引用，不做迁移；不重写 QGIS、单因素、预测或编图算法。
不宣称支持泛 WITSML 或任意 XML。综合 XML 的岩性/分层进入单因素属于已有
专用链路，需回归验证，不能用曲线适配冒充全部业务支持。保留未知轴语义和单位，
不猜测、不强制重采样。不同采样轴仅精确并集/NaN 空位，不插值。
不引入数据库服务器、规则语言或新基础依赖。
