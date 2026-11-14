# 本地模型声明发现

对应 [W6](../planning/AGENT_CALL_READINESS_PLAN.md) 的精确模型发现接缝。
`PaleoOnnxService::modelContractPage(name,cursor,trustedScope,pageSize,maxBytes)`
只读取既有、已安装的注册表快照；不重新 scan、不读取模型文件、不建立 ORT 会话。
聊天的 `query_project topic=local_models` 由组装后的 runner 委托此方法。

## 契约和边界

- 原 `m_registryOk` 改为 `QMap`，按 name 的稳定大小写敏感顺序 `upperBound` 续读；
  name 非空时精确筛选，不先全量枚举、排序或截断。没有增加另一份模型注册表。
- `paleo.local_models.page.v1` 返回 `models`，只投影逻辑名、版本、声明钉哈希、
  task/dataType、输入 name/dtype/shape、输出 semantics/classes 与有序 feature 声明。
  不返回 file、absolutePath、扫描错误 detail 或根目录。动态维保留内部声明值 -1。
- `entryFilter=manifest_status_ok`；`count` 为所有 Ok 唯一名称数，`matchedCount`
  为本查询匹配数，`rejectedCount` 为扫描中的拒绝记录数（包括无名、重名记录）。
  缺失或损坏 manifest 返回 `model_registry_unavailable`，不启用目录降级枚举。
- `metadataOnly=true`、`requires_prepare=true`、`runtimeVerified=false` 明确声明不是
  可运行性证明。未知科学 feature 值按声明返回，缺契约/缺钉哈希另列
  `declarationIssues`；实际数值、语义、特征和运行时验证仍以 prepare 为准。
- feature 单位保留原大小写；只允许声明字段 version/name/feature/kind/unit/domain/
  preprocessing/dtype，未知字段或不合法结构明确拒绝，不静默删除。
  先检查 64 个 feature、对象键、字符串及聚合字符预算，再检查完整 8192 字节上限。
- 页默认 16 条、8 KiB；接受 1–64 条和 1–32 KiB。计入游标和 coverage 的完整 UTF-8
  JSON 遵守字节预算。单条放不下明确拒绝，后续条目放不下时从最后已返回名称续页。
- 游标先检查原始字符串 4096 字符上限，随后验证 HMAC 和规范化编码；绑定可信
  scope 摘要、注册表 epoch、name、页预算及 after-key，TTL 为 30 分钟。
  无效、跨 scope、配置过期、查询不匹配与超时有独立错误码。
- 既有扫描读取改为最多 8 MiB+1 字节，处理 stat 后增长，并在处理任何模型文件前
  拒绝超过 4096 条的 manifest；不改变既有模型文件和哈希验证语义。

## 验证

生产 `paleo_ai_onnx` 与 `tst_modelcontractquery` 采用同一新 TU；测试没有备用实现。
Qt 6.11.2 / GCC 16.2.1 / vendored ONNX Runtime 1.30.0 独立实际配置、编译与链接
通过，Office 编辑器接线就绪。新增 7 个业务场景/9 个含 fixture 用例全过：
一万条按名小页、同游标重放、精确名称、scope/epoch/query/TTL、完整 UTF-8 页预算与
单条过大、扫描后删除实际模型文件仍能只读声明、不创建 ORT 会话、单位大小写、
缺契约/拒绝记录计数、超大 feature/未知路径字段，以及 manifest 读取和记录上限。

同一生产库的既有 `tst_modelregistry`（20/20）和 `tst_pinned_inference`（6/6）
同时通过，串行 CTest 共 3/3（0.41s），合计 35 通过、0 失败、0 跳过。
注册表回归按 `add_paleo_test` 改为最小 `paleo_ai_onnx` 链接集。
fetch 后完整 Source gates 32/32 通过，含锁定 QGIS 头的传递依赖检查；没有新增或
提高任何基线。候选只提交到独立工作分支，由 W6 累计集成并再次验证后交付 PR。
