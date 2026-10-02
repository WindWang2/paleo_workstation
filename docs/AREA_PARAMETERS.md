# 工区参数分界（AreaRules）

状态：wave4/area-parametrization 落地（TODOS「第二工区参数化接缝」，原 P3 提前
实施）。`src/domain/arearules.{h,cpp}` 是「每测区参数」的唯一权威来源（UI_LAYER_PLAN C-4 后由 io 平移到 domain）；本文回答
三件事：什么参数随工区变、配置文件怎么写、接入第二个工区要做什么。

## 1. 每测区参数 vs 全局规则

判据：换一个工区（不同数据目录、不同方言、不同网格）时**会不会想改**它。
会 → 每测区参数（AreaRules）；不会 → 全局规则（留在代码里）。

### 每测区参数（AreaRules 承载，默认 = 本工区 project_area 钉死值）

| 参数组 | 内容（本工区默认值） | 消费方 |
|---|---|---|
| `sequenceBoundaries` | 8 层序界面：`C3 C6 D53 D61 D62 D63 D71 D72`（**有序，浅→深**） | `dataimportservice.cpp` `isKnownSequenceBoundary`（名单外层位产未决实体）+ `domain/mappinghorizons.h`（编图 chip 集合、厚度基面=下一界面；`mappingHorizons()` 直通此表） |
| `targetHorizon` | `D61` | 标定层位：验证工作流时间残差、ONNX 结果落栅格的 `horizon.<target>` 声明前缀、时深 tie 井、相关面板文案。必须是 `sequenceBoundaries` 成员否则拒用 |
| `classifier.datPathRules` | `.dat` 路径段规则表（按序）：段 `td`/含「时深」→ `time_depth`；含「层位」→ `horizon`；含「井分层」→ `well_stratification`；含「井位」或文件名含 `wellhead`/`well_head` → `well_head`；段 `dev`/含「测斜」「井斜」或文件名含 `deviation`/`trajectory`/「井斜」→ `well_deviation`；全不中 → `tabular` | `projectclassifier.cpp` `classifyProjectPath` |
| `classifier.referenceDirNames` | 目录段「参考资料」→ 默认「参考」角色 | `projectclassifier.cpp` `isDefaultReferencePath` |
| `classifier.fixedAuxiliaryNameStem` | 基名含 `HZ28-6-1`（本工区井名）→ 固定辅助 | `projectclassifier.cpp` `isFixedAuxiliaryPath` |
| `segy`（道号索引约定） | 道头 0 基偏移：inline 字 188、crossline 字 192、field record 字 8（= 冻结 inlineMin，本工区 1315）、CDP 字 20（道号序 crossline 来源） | `segyreader.cpp` `SegyReader::open`（inline 字恒定 → 道号序：inline = base + 道号/N） |
| `onnxGrid` | 期望网格 411 行 × 641 列 | `workflows.cpp` ONNX 输出尺寸门（不符拒写栅格） |

### 全局规则（不随工区变，仍在代码里）

| 规则 | 位置 |
|---|---|
| 扩展名 → 类型表（`las`→well_log、`sgy/segy`→seismic、`geojson`、`dat` 段规则、`pdf/ppt(x)/doc(x)`→document、图片→image_reference、`xml` 内容嗅探） | `projectclassifier.cpp` `classifyProjectPath` / `classifyProjectImport` |
| 身份顺序（已有 id → UWI → 规范化井名 → 别名；文件名不作身份仅回退） | `dataimportservice.cpp` |
| 受管 RAW 语义（复制边算 SHA-256、落盘只读）、SEG-Y 一律外链、显式 `entity_asset_link` | `dataimportservice.cpp` |
| SEG-Y 二进制头解读（字节 13-14 每线道数、格式码 1/5、扩展文本头）、CDP 序列/角点一致性校验 | `segyreader.cpp` |
| 层位装箱（`# Grid_size:` 等文件头声明）——网格值来自**文件本身**，不是工程参数 | `horizonbinner.cpp` |
| 井文件解析列契约（SMI 列、-99999→空） | `wellfileparsers.cpp` |
| ONNX 输出 geotransform/SRS 的 D61 声明优先、写 Float32 GeoTIFF | `workflows.cpp` |

## 2. 配置 schema：`<工程目录>/project_area.json`

读取顺序（`AreaRules::loadFromProjectDir`）：**工程目录 `project_area.json`
→ 内置默认**。缺文件 = 默认；缺键 = 该键默认（`dat_path_rules` 若出现则
**整表替换**——优先级序是表语义，不与默认表合并）；坏 JSON（语法错/类型错/
未知键/越界值）→ `loadFromProjectDir` 返回 false + 具体原因，**拒用**（不静默
回退——`setProjectDir` 后 `AreaRules::lastError()` 可查，active 保持默认）。

未知键也是错：键名拼错（`boundaries` vs `sequence_boundaries`）若被静默忽略，
等于带病跑默认值。

```json
{
  "schema_version": 1,
  "sequence_boundaries": ["C3", "C6", "D53", "D61", "D62", "D63", "D71", "D72"],
  "target_horizon": "D61",
  "classifier": {
    "dat_path_rules": [
      {"exact_segments": ["td"], "segment_keywords": ["时深"], "type": "time_depth"},
      {"segment_keywords": ["层位"], "type": "horizon"},
      {"segment_keywords": ["井分层"], "type": "well_stratification"},
      {"segment_keywords": ["井位"], "filename_keywords": ["wellhead", "well_head"], "type": "well_head"},
      {"exact_segments": ["dev"], "segment_keywords": ["测斜", "井斜"], "filename_keywords": ["deviation", "trajectory", "井斜"], "type": "well_deviation"}
    ],
    "reference_dir_names": ["参考资料"],
    "fixed_auxiliary_name_stem": "HZ28-6-1"
  },
  "segy_indexing": {
    "inline_word_offset": 188,
    "crossline_word_offset": 192,
    "field_record_offset": 8,
    "cdp_xline_offset": 20
  },
  "onnx_grid": {"rows": 411, "cols": 641}
}
```

字段语义与校验：

- `schema_version`：可选，出现则必须为 `1`。
- `sequence_boundaries`：字符串数组；装载时统一大写（消费方对层位名取大写
  比较）；**数组顺序即地层序（浅→深）**——编图 chip 顺序与厚度基面推导
  （`baseHorizonFor` = 下一界面）都吃这个序。空数组 = 显式「无层序界面」
  （合法值，不是错误）。
- `target_horizon`：非空字符串，装载时统一大写；**必须是
  `sequence_boundaries` 的成员**——自定义边界表不写它、或写了表外名字，
  都按配置错误拒用（标定一个不存在的层位没有安全语义）。
- `classifier.dat_path_rules[]`：`type` 必填且必须在
  `projectClassifierTypes()` 词表内；三个匹配器列表（`exact_segments` 路径段
  精确相等、`segment_keywords` 路径段包含、`filename_keywords` 文件名包含，
  匹配大小写不敏感）至少给一个；匹配 = 任一命中，规则按表序先中先得。
- `reference_dir_names`：目录段**原文精确**匹配（不小写化）。
- `fixed_auxiliary_name_stem`：空串 = 显式关闭固定辅助。
- `segy_indexing.*`：0–236 的整数（i32 字完整落在 240 字节道头内）。
- `onnx_grid.rows/cols`：≥ 1。

## 3. 新增工区的操作步骤

1. 在新工程目录放 `project_area.json`（从 §2 模板抄，改掉与本工区不同的键；
   与默认相同的键可以不写）。
2. 核对五组值：层序界面名单（地质分层命名，浅→深序）、标定层位
   （`target_horizon`，必须是名单成员）、目录关键字（数据目录的中文
   命名习惯）、SEG-Y 道头方言（拿一个体看 inline 字偏移 188 是否恒定、CDP
   在哪个字——必要时用 `tools/make_segy_fixture.py` 造对照体）、ONNX 模型
   输出网格（行×列，模型侧定了就是它）。
   注：ONNX geotransform 兜底常量仍是本工区原点/像元——新工区先在清单里
   声明 `horizon.<target>*` 供体栅格（声明优先），再谈常量。
3. 工程打开时绑定：调 `AreaRules::setProjectDir(工程目录)`（接线点见 §4）。
   之后导入/分类/SEG-Y 打开自动按新规则走。
4. 验证：`tst_arearules` 的三组行为（默认等价/自定义生效/坏 JSON 拒用）是
   契约样例；新工区数据导入后查确认表分类是否符合预期。
5. 常见坑：
   - `.dat` 规则表整表替换——只加新关键字不改旧关键字的写法是把旧表**全部**
     抄进来再改；
   - SEG-Y inline 字**可变**的体走标准直读（188/192），`field_record_offset`/
     `cdp_xline_offset` 只在道号序（inline 字恒定）时参与；inline 字可变且
     不在 188 的方言是 SEG-Y 字节表编辑器（TODOS P3）的范围，本 seam 覆盖
     不了；
   - 坏 JSON 不会回退默认——`AreaRules::lastError()` 有内容就先修配置。

## 4. 接线点现状（本 wave 边界）

| 消费方 | 状态 |
|---|---|
| `classifyProjectPath` / `isFixedAuxiliaryPath` / `isDefaultReferencePath` | ✅ 读 `AreaRules::active()` |
| `isKnownSequenceBoundary` | ✅ 读 `AreaRules::active().sequenceBoundaries`（签名/调用点未动） |
| `SegyReader::open` | ✅ open() 起点取 `AreaRules::active().segy` 快照，全程一致（工程切换不撕裂单次打开） |
| ONNX 网格门（`src/workflow/workflows.cpp`） | ✅ 已接：行/列读 `active().onnxGrid`；geotransform 供体前缀 `horizon.<targetHorizon>`；provenance 同理。geotransform 常量仍为 project_area 兜底（注释已记） |
| `AreaRules::setProjectDir` 的工程打开时机 | ✅ 已接：`appcontext.cpp` `projectOpened` 里 `setProjectDir(dir)`，先于 importSvc/catalog；坏 JSON → `lastError()` 进 qWarning + QgsMessageLog，active 保持默认 |
| 层位集合（`mappingHorizons()`/`isMappingHorizon`/`baseHorizonFor`） | ✅ 已接：直通 `active().sequenceBoundaries`（序=浅→深）；chip 条 `reloadHorizons()` 在 projectOpened 重建 |
| 标定层位（验证时间残差、ONNX 声明前缀、时深 tie、面板文案） | ✅ 已接：`active().targetHorizon`；约束/验证/ONNX 面板文案在 projectOpened 重写（`paleomainwindow` 参数刷新块） |

线程语义：`active()` 返回值拷贝（互斥锁下快照）——导入/SEG-Y 索引在任务池
线程上读（D1），工程切换在主线程写，两者安全；单次 `SegyReader::open` 内
偏移恒定。

行为等价性证据：`tests/tst_arearules.cpp` 钉住默认表逐值 + 默认分类行为
（与 `tst_projectparsers::classifiesEachCategory` 同断言）+ 自定义 SEG-Y
方言体只有配对偏移才索引得动；`tst_import`/`tst_segy*`/`tst_smoke_realdata`
全绿即默认路径无回归。
