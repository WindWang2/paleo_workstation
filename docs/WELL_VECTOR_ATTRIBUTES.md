# 测井与单因素的矢量属性维护

测井页「岩性与相属性」、智能预测页同名按钮，打开 `welllog.attributes`
矢量属性表。测井页「地层优势相属性」、智能预测页「地层相属性」按钮，打开
`welllog.formations`（图层名「测井地层相属性」；列别名为地层相、相厚度、相厚度占比，
智能预测侧不出现「优势」字样）。单因素页「维护井点属性」以及测井页「井点因子属性」，打开
`wellfactor.attributes`。沿用底部属性表的「编辑 / 保存编辑 / 放弃编辑」。
井 ID 必须指向工程内已唯一解析的真实井；辅助井未唯一解析时禁用入口。
只读工程可以查看已有表，不能创建或更新表。

## 井段表

持久文件为 `artifacts/layers/well-log-attributes.gpkg`，`attributes` 图层：

| 字段 | 内容 |
|---|---|
| `well_id` / `well_name` | 稳定井身份 / 井名 |
| `top_md` / `base_md` | 顶底测深，单位 m；同井井段不能重叠 |
| `horizon` | 预测的层位上下文（单井未提供层位时保留已有上下文） |
| `lithology` / `lithology_pattern` | 岩性文字 / 图案键 |
| `facies` / `sub_facies` / `micro_facies` | 当前维护的相解释；用于井道显示 |
| `facies_pattern` | 相图案键 |
| `predicted_facies` | 最近一次预测的相文字；手工修订 `facies` 时保留 |
| `facies_code` | 批量预测的分类编码；单井自由词面预测清空旧编码 |
| `model` / `job_id` | 预测模型/方法与任务来源 |

首次打开工程井文件时，将已有岩性、相解释补入表的缺失字段。已有文字、
显式空字符串和自定义属性不会被源文件覆盖。没有维护相的行保留源文件的
相解释。修改属性并保存后，当前打开的测井页刷新岩性和维护相道；后续单井
推理使用表中岩性。原始预测和真实置信度道仍保留模型返回值。

单井与批量成功预测共用区间合并：根据新旧边界切分井段，只更新对应井、
对应深度内的相字段，保留岩性、其它井和自定义属性。所有井段预校验后在
同一编辑缓冲提交；无效结果不写入。属性表正在编辑时不能提交新预测。
批量测井的人工工作副本在「保存图件版本」后回写维护相，历史预测版本
保持原始内容。旧结果中的合成深度井段（`depth_mock`）不会进入维护表。

预测相与岩性是两类信息。砂厚读取 `lithology`，不将河道、三角洲等预测相
当作砂岩。原有解释岩性资产与岩屑文件仍可供尚无表中岩性的井读取。

## 井点因子表

持久文件为 `artifacts/layers/well-factors.gpkg`，`attributes` 图层。每口井、
每个层位只有一行，字段包括：

- `log_layer_thickness_md`：层厚，m。
- `log_sand_thickness_md`：砂厚，m。
- `sand_ratio`：砂地比，0–1。
- `porosity`、`permeability` 及用户增加的数值字段。

「维护井点属性」首次为当前层位补行：层厚来自真实分层 MD；砂厚来自完整、
无冲突的解释岩性覆盖；两者有效时以砂厚/层厚初始化砂地比。没有依据的
字段留空。已维护行（包括显式 NULL）不再自动重算；各字段可以分别维护。

提取/制图按 `well_id + horizon` 读取最新属性。维护的数值或 NULL 优先于
自动计算，零值有效，NULL 作为缺失报告。保存属性后字段下拉刷新并使旧提取
状态失效；重新提取生成新的不可变样点版本，再计算单因素图。可以直接选
`sand_ratio`，也可以显式选择砂厚/层厚做比值。零分母保持缺失。

两张表都独立于可再生成的 `wells.geojson`，因此导入资料、刷新井位不会
覆盖地质属性。其图层声明随工程保存，文件仍是普通可编辑 GeoPackage。

## 地层优势相表

持久文件为 `artifacts/layers/well-formation-facies.gpkg`，`attributes` 图层。
**每口井、每个地层单位（组、段各一行）只有一行**，几何是井位点，所以可以直接
在属性表里维护，也可以按属性列做图。这是数据管理（逐组预测）与智能预测（测井
预测）共用的一张表：两边都往这里写，两边都从这里读。

优势（微）相 = 地层内累计厚度最大的相。例如地层厚 100 m，分流河道累计 60 m，
河口坝 40 m，优势微相就是分流河道，占比 0.6。统计沿用相图合成的优势相口径
（`computeDominantFacies`）：观测按顶深排序，重叠段归先到者、不双算；越出地层
的观测按地层裁剪；并列取更浅者并标记 `tie`；没有任何相观测的地层不出行。

| 字段 | 内容 |
|---|---|
| `well_id` / `well_name` | 稳定井身份 / 井名 |
| `formation` / `unit_type` | 地层名 / 层级（组、段）；同井地层名唯一 |
| `top_md` / `base_md` / `thickness_md` | 地层顶底测深与厚度，m |
| `dominant_facies` | 优势微相文字，**维护列**，可手工修订 |
| `facies_code` | 优势相的分类编码（智能预测带编码；自由词面预测为空） |
| `predicted_dominant` | 最近一次预测得到的优势相；手工改 `dominant_facies` 时保留 |
| `dominant_thickness_md` / `dominant_ratio` | 优势相累计厚度 / 占已分类厚度比（0–1） |
| `coverage` | 已分类厚度 / 地层厚度（0–1），预测只覆盖部分地层时如实偏低 |
| `tie` | 优势并列标记 |
| `facies_composition` | 相厚度构成，如「分流河道 60.00 m；河口坝 40.00 m」 |
| `source` / `model` / `job_id` | 来源（`welllogfacies` = 数据管理，`wells_prediction` = 智能预测）/ 模型 / 任务 |

写入规则：按 `well_id + formation` 更新或新增，只覆盖给出的列，保留未给出的列
和自定义属性；整批在同一编辑缓冲提交，任一行无效（井未解析、地层名为空或重复、
顶底倒置、厚度为负、占比/覆盖率不在 0–1、相编码非 1–32767 整数）整批不写。
属性表正在编辑、只读工程时不能写入。

- 数据管理：逐组预测全部走完后，按井的地层道（组、段）对合并结果统计，与井段表
  一起回写；综合图同时多出「优势相」道（组、段两级都有时各一道，文字为优势微相
  与占比）。
- 智能预测：「测井 → 地层预测相点」完成后，按该井 tops 的地层单位（顶 = 本层位顶，
  底 = 下一层位顶，最末取预测底）对各层位井段统计后回写；地图井点的相取本层位那一行
  的优势相（同一口径）；没有 tops 的井没有
  地层可组织，不出行。人工修订预测井段并保存图件版本时，同步更新维护的
  `dominant_facies`，不动 `predicted_dominant`。
- 智能预测请求的每口井带 `vector_formations`（该井全部地层行，含手工维护值），
  服务端可作输入。

## 批量预测传输契约

既有智能预测的 `/health`、`/predict` 路由在 `kind=wells` 时提交 `wells` 与
`facies`。每口井包含冻结的 `vector_intervals`、当前层位 `vector_factors`、
该井全部地层行 `vector_formations`、曲线版本 ID 和深度范围。服务端可使用这些属性作为输入。

服务返回 `points` 数组，每个井点含 `id`、`x`、`y`、`facies_code` 与
`facies_intervals`；井段支持 JSON 数组或编码后的 JSON 字符串，其中各段为
`{top, bottom, code}`。工程侧检查井集合、坐标、相编码与深度覆盖后回写。
测井响应不走栅格本地降级。单井测井服务仍使用已有 API 2.0 模型契约。

验证使用本地 HTTP 服务及临时工程，未调用真实外网预测服务：

```sh
cmake --build build --target paleo tst_wellattributes tst_wellfacies \
  tst_wellfaciesconfig tst_factorworkflow tst_mappingworkbench \
  tst_remotepredictrouter tst_wellsection_workflow -j8
ctest --test-dir build -j8 --output-on-failure \
  -R '^(tst_(wellattributes|wellfacies|wellfaciesconfig|factorworkflow|mappingworkbench|remotepredictrouter|wellsection_workflow)|layering.*)$'
```
