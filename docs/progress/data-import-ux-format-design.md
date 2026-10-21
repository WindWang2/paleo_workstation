# 统一测井读取：源码审查与最小方案

- `WellLogRead` 已是 LAS/DLIS/LIS 的统一消费入口，`LasHeaderInfo` 保留井名与索引基准，`LasCurve` 保留单位和双精度采样；不新增领域模型。
- `wellcompositexml.cpp::parseComprehensiveWellXmlData` 处理实际 SpreadsheetML 综合柱状图，但坏深度会跳行，曲线使用 float，单位按 mnemonic 推断；直接转换其结果会掩盖错误并损失精度。
- `outsourceworkbook.cpp` 已提供 SpreadsheetML 字符串单元格读取、稀疏列处理、物理行号、格式问题列表与 4M 格预算。复用此读取器，所有格式问题作为整文件拒收原因；仅识别现有“测井曲线”/“离散曲线”工作表。
- 增加轻量文本适配器：UTF-8 CSV/TSV/空白分隔 TXT，要求明确索引表头，支持 `DEPTH[m]`、`GR[API]` 和 `# Well: A1`。缺失值只识别空白、NA、NAN、NULL、-999.25/-9999；其他非法数字报物理行号，不静默跳过。
- 原始文件只读，单位和 MD/TVD/TIME/TVDSS 原样保留，不转换未知单位。不同 XML 工作表仅同基准同单位时以精确索引并集和 NaN 对齐，不插值；拒绝重复/逆序索引、混井、重复曲线名和密集输出超预算。
- XML 井身份允许来自井名/井号列，或明确的 `测井曲线-A1井` / `离散曲线-A1井` 工作表名（仅移除最后一个“井”）。无标记后缀不猜；各表身份与井名列必须完全一致，否则整文件拒收。
- 内容检测先于扩展名；未知 XML/未知文本明确失败。LAS/DLIS/LIS 继续调用原解析器，不改变二进制能力。
- 不解析通用 WITSML，不把岩性、层段等非曲线工作表伪造成测井曲线；这些继续由现有综合柱状图工作流消费。不引入依赖、持久化迁移、重采样或新格式规则系统。

## 已确认的消费面接入修复

`propertymodelworkflow.cpp::requestFromCatalog` 和 `sectionworkbench.cpp::sectionWells` 已从 `WellLogSet::wellCurveIndex` 取得真实来源引用，但仍直接调用 LAS 解析器，导致新增 XML/文本数据被丢弃。两处改用 `WellLogRead::parse`，继续使用原有列号、单位换算、轨迹和算法。非 LAS 来源必须明确声明 MD 与可识别深度单位，否则属性建模返回错误、剖面对齐状态显示拒绝原因。

`mappingworkbench_predict.cpp` 的后台范围读取和 `wellpredictionreview.cpp::predictionLog` 同样直接消费目录原源版本，已统一读取。预测准备遇新来源的未知/非 MD 轴或单位时通过现有错误信号中止，避免落入假深度范围；预测对比读取将明确 MD 的英尺轴转换为米后提供给原有 MD 图道。传统 LAS 维持既有消费约定。

第二轮消费审查还确认 `wellsectionworkflow.cpp::loadCurveBodies`、`wellpredictionservice.cpp::prepare` 和 `crossplotsources.cpp::load` 缺少新来源轴约束。连井剖面拒绝非 MD 并通过原 warnings 显示原因；直接预测服务按文件一次读取，拒绝坏轴/缺单位的新来源并在结果的 `prediction_reason` 保留原因，整井不提交不完整输入；交会先将已知深度单位统一为 MD 米，无法声明的新来源返回错误，避免米/英尺或 TIME/TVD 裸索引配对。未改交会插值算法或预测算法；LAS 既有无单位兼容约定保留。
