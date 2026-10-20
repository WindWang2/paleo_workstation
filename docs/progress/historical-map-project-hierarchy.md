# 已有古地理图与工区三级分类对齐（2026-10-20）

实际数据：`Downloads/paleo_comp_data/historical_facies_map`，D61/D62/D63
三份 SHP 共 13/9/10 个面。旧字段 `microfac` 未被自动整理器识别；原图
4/5/6/7/8/9 等编码与工区同号类别不同。原 QML 使用独立平级色板，目录
仅有 RAW 图件，缺少带工区三级分类和纹理的 DERIVED。

## 修复

- `PaleoMapSource` 自动识别 microfac，按完整三级路径对照工区分类，
  保留已有编码。原图编码和颜色作为 source_* 字段追溯；输出明确的
  facies_code/facies_name/subfacies/microfacies/facies_label/texture/color。
  遇到已存在的 source_* 字段采用不冲突的名称。
- 整理策略版本升为 2，旧策略缓存不会阻止新字段及外观规则生效。
- 工区原 1–7 微相编码不变；采用系统模板补入 10 浅湖泥、11 沼泽沉积、
  15 河漫滩，八个层位保持同一套 10 类三级架构。
- 原始 SHP 属性、QML、四张 PNG 更新；新增三份完整字段 GPKG。
  受管旧 RAW 保留，三份整理结果和四张预览登记新版本。
- SQLite/JSON 目录、图层清单、QGZ 内嵌清单、project.paleo 同步。
  工程改用 product.ver-602/604/606，D62/D63 初始不勾选。
- Windows 清理扫描统一路径大小写身份，避免 RAW/raw 枚举把被引用的
  QML 误列为孤立文件。Linux 继续区分大小写。

## 验收

- `tst_project_paleomaps` 使用生产 PaleoMapLibrary 和 MappingWorkbench
  整理实际工区副本：32 个面逐面比较几何、三级字段、编码、颜色、纹理。
- 正式工区重新打开后实际渲染三份图件，15 个图层全部有效。
- 全部 401 个版本文件存在、SHA-256 一致；原 386 个版本保留。
  原 SHP 几何文件 SHA 和 32 个面的 WKB 与备份相同；原工程 CRS 和
  转换上下文未改变，新图件使用工程相对数据源，无处理副本路径引用。
- 文件与工程备份：
  `Downloads/paleo_comp_data_before_paleomap_repair_20261020-074929`。
- 工区内交付 `已有古地理图更新说明.md`、`古地理图更新记录.json`；
  本地验收日志 `build/paleomap-project-final-acceptance.log`，摘要
  `build/paleomap-repair-integrity.json`。

库全量回归中 21 项通过，1 项 Windows 导出目录提交仍失败：
`shapefileIsOneImmutableDatasetAndDbfChangesCreateAVersion` 的 QML 保活
检查已通过，后续 `DataExport::execute` 的目录 rename 失败。其导出路径
未用于本次数据写回；未通过更改预期或扩大基线掩盖。独立 storagegovernance
回归通过。layering strict 和 diff whitespace 通过。

新增数据验收测试默认跳过；只读验证设置 PALEO_PALEOMAP_VERIFY_PROJECT，
带写入的整理仅在显式设置 PALEO_PALEOMAP_REPAIR_PROJECT 时执行。

## 预览重复标注修复（2026-10-20）

数据预览页在三级渲染后强制套用模式 3（名称＋相名）；原图 name 与微相
名称一致，因而每块面出现两行相同文字。工程中三份面图原已保存模式 2，
无需修改几何或再次生成数据版本。

- 预览按几何采用公共默认：面只标相名，井点保留井名＋相名；连续单因素
  面保留区间标注。显式组合标注遇到名称与相名相同，也只输出一次。
- `tst_previewmap_assets` 四项定向回归通过；实际工区只读打开，逐个验证
  32 个面表达式等于微相名称，并输出三份非空原生预览。
- 验收图：`build/single-label-previews/D61-single-label.png` 等；日志：
  `build/duplicate-label-tests.log`。layering strict、diff whitespace 通过。
- 便携版与 build/portable 已同步，SHA-256：
  `5ADFF693FC53456F9F26A8FDCC4C1DC70CAED15B22A703C1C19519FD80C4692D`。
  保留正在运行的旧进程与旧 exe 备份，重启后使用新程序。
